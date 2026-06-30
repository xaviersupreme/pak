#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


class TestFailure(Exception):
    pass


def fail(message):
    raise TestFailure(message)


def check(condition, message):
    if not condition:
        fail(message)


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def write_bytes(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def pseudo_random_bytes(size):
    state = 0x12345678
    data = bytearray()
    for _ in range(size):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        data.append(state & 0xFF)
    return bytes(data)


def run_pak(pak, cwd, *args, check_rc=True):
    env = os.environ.copy()
    env["NO_COLOR"] = "1"
    env.pop("FORCE_COLOR", None)
    result = subprocess.run(
        [str(pak), *[str(arg) for arg in args]],
        cwd=str(cwd),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if check_rc and result.returncode != 0:
        fail(
            "pak {args} exited {code}\nstdout:\n{stdout}\nstderr:\n{stderr}".format(
                args=" ".join(str(arg) for arg in args),
                code=result.returncode,
                stdout=result.stdout,
                stderr=result.stderr,
            )
        )
    return result


def list_names(pak, cwd, archive, *patterns):
    result = run_pak(pak, cwd, "list", archive, *patterns)
    names = []
    for line in result.stdout.splitlines():
        if line.startswith("==> "):
            continue
        match = re.match(r"^(.*?)\s{2,}\d+ bytes$", line.rstrip())
        if match:
            names.append(match.group(1).rstrip())
    return names


def list_methods(pak, cwd, archive):
    result = run_pak(pak, cwd, "list", "--full-name", archive)
    methods = {}
    current = None
    for raw_line in result.stdout.splitlines():
        line = raw_line.rstrip()
        if not line or line.startswith("==> ") or line in ("entries", "-------"):
            continue
        if not line.startswith("  "):
            current = line
            continue
        match = re.search(r"\bmethod\s+(\w+)", line)
        if current is not None and match:
            methods[current] = match.group(1)
    return methods


def assert_names(pak, cwd, archive, expected):
    actual = set(list_names(pak, cwd, archive))
    check(actual == set(expected), "list mismatch\nexpected: {0}\nactual:   {1}".format(sorted(expected), sorted(actual)))


def test_core_command_flow(pak, root):
    cwd = root / "core"
    cwd.mkdir()
    write_text(cwd / "alpha.txt", "alpha\n")
    write_text(cwd / "beta.txt", "beta v1\n")

    run_pak(pak, cwd, "make", "bundle", "alpha.txt", "beta.txt")
    archive = cwd / "bundle.pak"
    check(archive.exists(), "make should append .pak")
    assert_names(pak, cwd, archive, ["alpha.txt", "beta.txt"])

    cat = run_pak(pak, cwd, "cat", archive, "alpha.txt")
    check(cat.stdout == "alpha\n", "cat returned unexpected data: {0!r}".format(cat.stdout))

    write_text(cwd / "beta.txt", "beta v2\n")
    write_text(cwd / "gamma.txt", "gamma\n")
    run_pak(pak, cwd, "update", archive, "beta.txt", "gamma.txt")
    check(run_pak(pak, cwd, "cat", archive, "beta.txt").stdout == "beta v2\n", "update did not replace beta.txt")

    run_pak(pak, cwd, "rename", archive, "gamma.txt", "docs/gamma.txt")
    run_pak(pak, cwd, "delete", archive, "alpha.txt")
    assert_names(pak, cwd, archive, ["beta.txt", "docs/gamma.txt"])

    checked = run_pak(pak, cwd, "check", archive)
    check("ok: checked 2 files" in checked.stdout, "check did not report a clean 2-file archive")

    out_dir = cwd / "out"
    run_pak(pak, cwd, "unpack", archive, "-C", out_dir)
    check((out_dir / "beta.txt").read_text(encoding="utf-8") == "beta v2\n", "unpack wrote wrong beta.txt")
    check((out_dir / "docs" / "gamma.txt").read_text(encoding="utf-8") == "gamma\n", "unpack missed renamed path")


def test_directory_wildcards_and_mixed_inputs(pak, root):
    cwd = root / "inputs"
    cwd.mkdir()
    write_text(cwd / "loose.txt", "loose\n")
    write_text(cwd / "assets" / "config.json", "{\"ok\":true}\n")
    write_text(cwd / "assets" / "levels" / "one.map", "level one\n")
    write_text(cwd / "assets" / "readme.tmp", "temporary\n")
    write_text(cwd / "docs" / "guide.txt", "guide\n")

    archive = cwd / "mixed.pak"
    run_pak(pak, cwd, "make", "--paths", archive, "assets", "*.txt", "docs/*.txt")
    assert_names(
        pak,
        cwd,
        archive,
        ["config.json", "levels/one.map", "readme.tmp", "loose.txt", "docs/guide.txt"],
    )

    txt_names = set(list_names(pak, cwd, archive, "*.txt"))
    check(txt_names == {"loose.txt", "docs/guide.txt"}, "wildcard list selected {0}".format(sorted(txt_names)))

    selected = cwd / "selected"
    run_pak(pak, cwd, "unpack", archive, "levels/*", "-C", selected)
    check((selected / "levels" / "one.map").read_text(encoding="utf-8") == "level one\n", "wildcard unpack missed level")
    check(not (selected / "config.json").exists(), "wildcard unpack extracted an unselected file")

    run_pak(pak, cwd, "delete", archive, "*.tmp")
    run_pak(pak, cwd, "rename", archive, "docs/guide.txt", "docs/readme.txt")
    assert_names(pak, cwd, archive, ["config.json", "levels/one.map", "loose.txt", "docs/readme.txt"])

    if os.name == "nt":
        write_text(cwd / "win path" / "sub dir" / "note.txt", "windows path\n")
        run_pak(pak, cwd, "update", "--paths", archive, r"win path\sub dir\note.txt")
        names = set(list_names(pak, cwd, archive))
        check("win path/sub dir/note.txt" in names, "Windows backslash path was not normalized into the archive")


def test_absolute_file_and_directory_names(pak, root):
    cwd = root / "absolute"
    cwd.mkdir()
    write_text(cwd / "loose.txt", "loose absolute\n")
    write_text(cwd / "assets" / "config.txt", "config\n")

    archive = cwd / "absolute.pak"
    run_pak(pak, cwd, "make", archive, cwd / "loose.txt", cwd / "assets")
    assert_names(pak, cwd, archive, ["loose.txt", "config.txt"])

    cat = run_pak(pak, cwd, "cat", archive, "loose.txt")
    check(cat.stdout == "loose absolute\n", "absolute file input was not stored by base name")


def test_repack_and_compression_smart_skip(pak, root):
    cwd = root / "compress"
    cwd.mkdir()
    write_bytes(cwd / "repeat.txt", (b"repeat me\n" * 5000))
    write_bytes(cwd / "image.png", b"A" * 4096)
    write_bytes(cwd / "fake.png", b"A" * (2 * 1024 * 1024))
    write_bytes(cwd / "noisy.bin", pseudo_random_bytes(1100 * 1024))

    archive = cwd / "compressed.pak"
    made = run_pak(pak, cwd, "make", "--compress", archive, "repeat.txt", "image.png", "noisy.bin")
    check("store image.png: known compressed file type" in made.stdout, "smart compression did not skip .png by type")
    check("store noisy.bin: sample deflate saved" in made.stdout, "smart compression did not sample-skip weak large input")

    methods = list_methods(pak, cwd, archive)
    check(methods.get("repeat.txt") in ("deflate", "rle"), "repeat.txt should be compressed, got {0}".format(methods.get("repeat.txt")))
    check(methods.get("image.png") == "store", "image.png should be stored, got {0}".format(methods.get("image.png")))
    check(methods.get("noisy.bin") == "store", "noisy.bin should be stored, got {0}".format(methods.get("noisy.bin")))

    run_pak(pak, cwd, "repack", "--store", archive, "repeat.txt")
    methods = list_methods(pak, cwd, archive)
    check(methods.get("repeat.txt") == "store", "repack --store did not store repeat.txt")

    run_pak(pak, cwd, "repack", archive, "repeat.txt")
    methods = list_methods(pak, cwd, archive)
    check(methods.get("repeat.txt") in ("deflate", "rle"), "repack did not recompress repeat.txt")

    forced = cwd / "forced.pak"
    run_pak(pak, cwd, "make", "--no-smart-compress", forced, "fake.png")
    methods = list_methods(pak, cwd, forced)
    check(methods.get("fake.png") == "deflate", "--no-smart-compress did not bypass extension skip")


TESTS = [
    ("core command flow", test_core_command_flow),
    ("directory, wildcard, mixed input flow", test_directory_wildcards_and_mixed_inputs),
    ("absolute file and directory naming", test_absolute_file_and_directory_names),
    ("repack and compression smart-skip flow", test_repack_and_compression_smart_skip),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pak", required=True, help="path to the pak executable")
    args = parser.parse_args()

    pak = Path(args.pak).resolve()
    check(pak.exists(), "pak executable not found: {0}".format(pak))

    failures = 0
    print("1..{0}".format(len(TESTS)))
    with tempfile.TemporaryDirectory(prefix="pak-cli-tests-") as tmp:
        root = Path(tmp)
        for index, (name, test) in enumerate(TESTS, 1):
            try:
                test(pak, root)
                print("ok {0} - {1}".format(index, name))
            except Exception as exc:
                failures += 1
                print("not ok {0} - {1}".format(index, name))
                print("  {0}".format(str(exc).replace("\n", "\n  ")))

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
