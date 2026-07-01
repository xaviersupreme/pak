#!/usr/bin/env python3
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
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


def run_pak_bytes(pak, cwd, *args, check_rc=True):
    env = os.environ.copy()
    env["NO_COLOR"] = "1"
    env.pop("FORCE_COLOR", None)
    result = subprocess.run(
        [str(pak), *[str(arg) for arg in args]],
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if check_rc and result.returncode != 0:
        fail(
            "pak {args} exited {code}\nstdout:\n{stdout}\nstderr:\n{stderr}".format(
                args=" ".join(str(arg) for arg in args),
                code=result.returncode,
                stdout=result.stdout.decode(errors="replace"),
                stderr=result.stderr.decode(errors="replace"),
            )
        )
    return result


def run_pak_tty(pak, cwd, *args, input_text="y\n", timeout=10):
    if os.name == "nt":
        return None

    import errno
    import pty
    import select
    import time

    env = os.environ.copy()
    env["NO_COLOR"] = "1"
    env.pop("FORCE_COLOR", None)

    master, slave = pty.openpty()
    proc = subprocess.Popen(
        [str(pak), *[str(arg) for arg in args]],
        cwd=str(cwd),
        env=env,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)

    output = bytearray()
    sent_input = False
    deadline = time.time() + timeout
    while True:
        if time.time() > deadline:
            proc.kill()
            os.close(master)
            fail("pak {0} timed out in tty run\noutput:\n{1}".format(" ".join(str(arg) for arg in args), output.decode(errors="replace")))

        ready, _, _ = select.select([master], [], [], 0.1)
        if ready:
            try:
                chunk = os.read(master, 4096)
            except OSError as exc:
                if exc.errno != errno.EIO:
                    os.close(master)
                    raise
                chunk = b""

            if chunk:
                output.extend(chunk)
                if input_text is not None and not sent_input and (b"[Y/n]" in output or b"[Y/n/d]" in output):
                    os.write(master, input_text.encode("utf-8"))
                    sent_input = True
            elif proc.poll() is not None:
                break

        if proc.poll() is not None:
            while True:
                ready, _, _ = select.select([master], [], [], 0)
                if not ready:
                    break
                try:
                    chunk = os.read(master, 4096)
                except OSError:
                    break
                if not chunk:
                    break
                output.extend(chunk)
            break

    os.close(master)
    return subprocess.CompletedProcess([str(pak), *[str(arg) for arg in args]], proc.returncode, output.decode(errors="replace"), "")


def pak2_entries(path):
    data = bytearray(path.read_bytes())
    check(data[:4] == b"PAK2", "test archive is not PAK2")
    count = struct.unpack_from("<I", data, 4)[0]
    pos = 8
    entries = []
    for _ in range(count):
        header_offset = pos
        name_len, flags, size, stored_size, checksum = struct.unpack_from("<IIQQI", data, pos)
        name_offset = pos + 28
        data_offset = name_offset + name_len
        name = data[name_offset:data_offset].decode("utf-8")
        entries.append(
            {
                "name": name,
                "flags": flags,
                "size": size,
                "stored_size": stored_size,
                "checksum": checksum,
                "header_offset": header_offset,
                "crc_offset": pos + 24,
                "data_offset": data_offset,
                "data_end": data_offset + stored_size,
            }
        )
        pos = data_offset + stored_size
    return data, entries


def pak2_stored_entry(name, payload):
    name_bytes = name.encode("utf-8")
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    return struct.pack("<IIQQI", len(name_bytes), 0, len(payload), len(payload), zlib.crc32(payload) & 0xFFFFFFFF) + name_bytes + payload


def write_pak2_archive(path, entries):
    data = bytearray(b"PAK2" + struct.pack("<I", len(entries)))
    for name, payload in entries:
        data.extend(pak2_stored_entry(name, payload))
    path.write_bytes(data)


def make_check_archive(pak, cwd, archive_name):
    write_text(cwd / "alpha.txt", "alpha\n")
    write_text(cwd / "bravo.txt", "bravo\n")
    write_text(cwd / "charlie.txt", "charlie\n")
    archive = cwd / archive_name
    run_pak(pak, cwd, "make", archive, "alpha.txt", "bravo.txt", "charlie.txt")
    return archive


def assert_damaged_check(pak, cwd, archive, *needles):
    checked = run_pak(pak, cwd, "check", archive, check_rc=False)
    combined = checked.stdout + checked.stderr
    check(checked.returncode != 0, "corrupt archive should fail check")
    check("check report" in checked.stdout, "damaged check did not print a report\n" + combined)
    check("repair plan" in checked.stdout, "damaged check did not print a repair plan\n" + combined)
    for needle in needles:
        check(needle in combined, "damaged check did not report {0!r}\n{1}".format(needle, combined))
    return checked


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

    for version_arg in ("--version", "--v", "-v", "-V", "version"):
        version = run_pak(pak, cwd, version_arg)
        check(version.stdout.startswith("pak "), "{0} did not print a pak version".format(version_arg))

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


def test_missing_input_error(pak, root):
    cwd = root / "missing-input"
    cwd.mkdir()

    result = run_pak(pak, cwd, "make", "missing.pak", "to-remove.txt", check_rc=False)
    combined = result.stdout + result.stderr
    check(result.returncode != 0, "missing input should fail")
    check("cannot pack 'to-remove.txt': not found" in combined, "missing input error was unclear\n" + combined)


def test_windows_reparse_directory_is_skipped(pak, root):
    if os.name != "nt":
        return

    cwd = root / "windows-reparse"
    cwd.mkdir()
    write_text(cwd / "real" / "inside.txt", "inside\n")
    write_text(cwd / "loose.txt", "loose\n")

    link = cwd / "link"
    created = subprocess.run(
        ["cmd", "/c", "mklink", "/J", str(link), str(cwd / "real")],
        cwd=str(cwd),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if created.returncode != 0:
        return

    archive = cwd / "dot.pak"
    run_pak(pak, cwd, "make", "--paths", archive, ".")
    names = set(list_names(pak, cwd, archive))
    check("loose.txt" in names, "normal file was not packed")
    check("real/inside.txt" in names, "real directory was not packed")
    check("link/inside.txt" not in names, "reparse directory was followed")


def test_hostile_names_and_binary_stdout(pak, root):
    cwd = root / "hostile-names"
    cwd.mkdir()
    binary = b"\x00\x01pak\xff\r\n"
    write_bytes(cwd / "empty.bin", b"")
    write_text(cwd / "space name.txt", "space\n")
    write_text(cwd / "-dash.txt", "dash\n")
    write_bytes(cwd / "binary.bin", binary)

    run_pak(pak, cwd, "make", "names", "--", "empty.bin", "space name.txt", "-dash.txt", "binary.bin")
    archive = cwd / "names.pak"
    check(archive.exists(), "make should append .pak for hostile name case")
    assert_names(pak, cwd, archive, ["-dash.txt", "binary.bin", "empty.bin", "space name.txt"])

    dash = run_pak(pak, cwd, "cat", archive, "--", "-dash.txt")
    check(dash.stdout == "dash\n", "cat could not read leading-dash entry")
    empty = run_pak_bytes(pak, cwd, "cat", archive, "empty.bin")
    check(empty.stdout == b"", "cat changed empty file output")
    cat = run_pak_bytes(pak, cwd, "cat", archive, "binary.bin")
    check(cat.stdout == binary, "cat did not preserve binary stdout bytes")


def test_pakignore_and_invalid_flags(pak, root):
    cwd = root / "pakignore"
    cwd.mkdir()
    write_text(cwd / ".pakignore", "*.tmp\nignored/\n")
    write_text(cwd / "assets" / "keep.txt", "keep\n")
    write_text(cwd / "assets" / "skip.tmp", "skip\n")
    write_text(cwd / "assets" / "ignored" / "hidden.txt", "hidden\n")

    archive = cwd / "ignored.pak"
    run_pak(pak, cwd, "make", "--paths", archive, "assets")
    assert_names(pak, cwd, archive, ["keep.txt"])

    all_archive = cwd / "all.pak"
    run_pak(pak, cwd, "make", "--paths", "--no-pakignore", all_archive, "assets")
    assert_names(pak, cwd, all_archive, ["ignored/hidden.txt", "keep.txt", "skip.tmp"])

    bad = run_pak(pak, cwd, "list", "--overwrite", archive, check_rc=False)
    combined = bad.stdout + bad.stderr
    check(bad.returncode != 0, "list should reject extract-only flag")
    check("list: option '--overwrite' does not apply" in combined, "invalid flag diagnostic was unclear\n" + combined)


def test_extract_corrupt_payload_preserves_outputs(pak, root):
    cwd = root / "corrupt-extract"
    cwd.mkdir()
    write_text(cwd / "a.txt", "good\n")
    archive = cwd / "good.pak"
    run_pak(pak, cwd, "make", archive, "a.txt")

    data, entries = pak2_entries(archive)
    check(entries[0]["stored_size"] > 0, "test payload should not be empty")
    data[entries[0]["data_offset"]] ^= 0x55
    bad_archive = cwd / "bad.pak"
    bad_archive.write_bytes(data)

    existing = cwd / "existing"
    write_text(existing / "a.txt", "keep\n")
    result = run_pak(pak, cwd, "unpack", "--overwrite", bad_archive, "-C", existing, check_rc=False)
    combined = result.stdout + result.stderr
    check(result.returncode != 0, "corrupt extract should fail")
    check("checksum mismatch for 'a.txt'" in combined, "corrupt extract did not report checksum mismatch\n" + combined)
    check((existing / "a.txt").read_text(encoding="utf-8") == "keep\n", "corrupt extract overwrote existing output")
    check(not list(existing.glob("*.tmp*")), "corrupt extract left a temp file")

    fresh = cwd / "fresh"
    fresh.mkdir()
    result = run_pak(pak, cwd, "unpack", bad_archive, "-C", fresh, check_rc=False)
    check(result.returncode != 0, "corrupt extract to fresh directory should fail")
    check(not (fresh / "a.txt").exists(), "corrupt extract left a new bad output file")
    check(not list(fresh.glob("*.tmp*")), "corrupt fresh extract left a temp file")


def test_duplicate_archive_names_are_rejected(pak, root):
    cwd = root / "duplicate-names"
    cwd.mkdir()
    archive = cwd / "duplicate.pak"
    write_pak2_archive(archive, [("x.txt", "one\n"), ("x.txt", "two\n")])

    listed = run_pak(pak, cwd, "list", archive, check_rc=False)
    combined = listed.stdout + listed.stderr
    check(listed.returncode != 0, "list should reject duplicate archive names")
    check("duplicate archive name 'x.txt'" in combined, "list duplicate diagnostic was unclear\n" + combined)

    out = cwd / "out"
    out.mkdir()
    unpacked = run_pak(pak, cwd, "unpack", archive, "-C", out, check_rc=False)
    combined = unpacked.stdout + unpacked.stderr
    check(unpacked.returncode != 0, "unpack should reject duplicate archive names")
    check("duplicate archive name 'x.txt'" in combined, "unpack duplicate diagnostic was unclear\n" + combined)
    check(not (out / "x.txt").exists(), "duplicate archive preflight still wrote output")

    checked = run_pak(pak, cwd, "check", archive, check_rc=False)
    combined = checked.stdout + checked.stderr
    check(checked.returncode != 0, "check should report duplicate archive names as damage")
    check("duplicate entry name" in combined, "check did not report duplicate entry name\n" + combined)
    check("repair plan" in checked.stdout, "check did not offer a duplicate-name repair plan\n" + combined)


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


def test_check_damage_report(pak, root):
    cwd = root / "check"
    cwd.mkdir()
    write_text(cwd / "alpha.txt", "alpha\n")
    write_text(cwd / "beta.txt", "beta\n")

    archive = cwd / "broken.pak"
    run_pak(pak, cwd, "make", archive, "alpha.txt", "beta.txt")

    with archive.open("ab") as fp:
        fp.write(b"junk after archive")

    checked = run_pak(pak, cwd, "check", archive, check_rc=False)
    combined = checked.stdout + checked.stderr
    check(checked.returncode != 0, "corrupt archive should fail check")
    check("check report" in checked.stdout, "damaged check did not print a report")
    check("damage" in checked.stdout, "damaged check did not list damage")
    check("repair plan" in checked.stdout, "damaged check did not print a repair plan")
    check("archive end" in combined, "damaged check did not name the damaged archive area")
    check("rerun in a terminal to attempt repair" in combined, "noninteractive repair hint was missing")


def test_check_corruption_cases(pak, root):
    cwd = root / "check-cases"
    cwd.mkdir()

    header = make_check_archive(pak, cwd, "bad-header.pak")
    data, _entries = pak2_entries(header)
    data[0] = ord("X")
    header.write_bytes(data)
    assert_damaged_check(pak, cwd, header, "archive header is unreadable")

    truncated = make_check_archive(pak, cwd, "truncated.pak")
    data, entries = pak2_entries(truncated)
    del data[entries[-1]["data_offset"] + 2 :]
    truncated.write_bytes(data)
    assert_damaged_check(pak, cwd, truncated, "charlie.txt", "entry data is truncated")

    deleted = make_check_archive(pak, cwd, "deleted-middle.pak")
    data, entries = pak2_entries(deleted)
    check(entries[1]["stored_size"] >= 4, "second test payload was too small")
    del data[entries[1]["data_offset"] + 1 : entries[1]["data_offset"] + 3]
    deleted.write_bytes(data)
    assert_damaged_check(pak, cwd, deleted, "bravo.txt", "entry failed decompression or crc")

    trailing = make_check_archive(pak, cwd, "trailing.pak")
    with trailing.open("ab") as fp:
        fp.write(b"junk after archive")
    assert_damaged_check(pak, cwd, trailing, "archive end", "trailing junk after last entry")

    bad_crc = make_check_archive(pak, cwd, "bad-crc.pak")
    data, entries = pak2_entries(bad_crc)
    old_crc = struct.unpack_from("<I", data, entries[0]["crc_offset"])[0]
    struct.pack_into("<I", data, entries[0]["crc_offset"], old_crc ^ 0xFFFFFFFF)
    bad_crc.write_bytes(data)
    assert_damaged_check(pak, cwd, bad_crc, "alpha.txt", "entry failed decompression or crc")


def test_check_reports_multiple_scan_issues(pak, root):
    cwd = root / "check-multiple"
    cwd.mkdir()
    write_text(cwd / "alpha.txt", "alpha\n")
    write_text(cwd / "bravo.txt", "bravo\n")
    write_text(cwd / "charlie.txt", "charlie\n")

    archive = cwd / "broken.pak"
    run_pak(pak, cwd, "make", archive, "alpha.txt", "bravo.txt", "charlie.txt")

    data = bytearray(archive.read_bytes())
    data[0] = ord("X")
    pos = 8
    payload_offsets = []
    for _ in range(3):
        name_len, flags, size, stored_size, _crc = struct.unpack_from("<IIQQI", data, pos)
        pos += 28 + name_len
        payload_offsets.append((pos, stored_size))
        pos += stored_size
        check(flags == 0 and size == stored_size, "test archive should use stored entries")
    offset, stored_size = payload_offsets[1]
    check(stored_size >= 4, "second test payload was too small")
    del data[offset + 1 : offset + 3]
    archive.write_bytes(data)

    checked = run_pak(pak, cwd, "check", archive, check_rc=False)
    combined = checked.stdout + checked.stderr
    check(checked.returncode != 0, "multi-corrupt archive should fail check")
    check("2 issues" in checked.stdout or "3 issues" in checked.stdout, "check did not report multiple issues\n" + combined)
    check("archive header is unreadable" in combined, "check did not report the bad archive header")
    check("archive gap" in combined, "check did not report the damaged byte gap")


def test_check_repair_writes_clean_archive(pak, root):
    if os.name == "nt":
        return

    cwd = root / "check-repair"
    cwd.mkdir()
    archive = make_check_archive(pak, cwd, "repair.pak")
    with archive.open("ab") as fp:
        fp.write(b"junk after archive")

    repaired = cwd / "repair.repaired.pak"
    result = run_pak_tty(pak, cwd, "check", archive, input_text="d\ny\n")
    check(result is not None, "tty repair test needs pty support")
    check(result.returncode == 0, "repair check failed\nstdout:\n{0}".format(result.stdout))
    check(repaired.exists(), "repair did not write repaired archive")
    check("repair details" in result.stdout, "repair details were not shown\n" + result.stdout)
    check("keep" in result.stdout and "alpha.txt" in result.stdout, "repair details did not show kept files\n" + result.stdout)
    check("strip" in result.stdout and "archive end" in result.stdout, "repair details did not show stripped junk\n" + result.stdout)
    check("repaired: wrote" in result.stdout, "repair did not report repaired output\n" + result.stdout)

    checked = run_pak(pak, cwd, "check", repaired)
    check("ok: checked 3 files" in checked.stdout, "repaired archive did not pass check")


TESTS = [
    ("core command flow", test_core_command_flow),
    ("directory, wildcard, mixed input flow", test_directory_wildcards_and_mixed_inputs),
    ("absolute file and directory naming", test_absolute_file_and_directory_names),
    ("missing input error", test_missing_input_error),
    ("windows reparse directory skip", test_windows_reparse_directory_is_skipped),
    ("hostile names and binary stdout", test_hostile_names_and_binary_stdout),
    ("pakignore and invalid flags", test_pakignore_and_invalid_flags),
    ("extract corrupt payload preserves outputs", test_extract_corrupt_payload_preserves_outputs),
    ("duplicate archive names are rejected", test_duplicate_archive_names_are_rejected),
    ("repack and compression smart-skip flow", test_repack_and_compression_smart_skip),
    ("check damage report", test_check_damage_report),
    ("check corruption cases", test_check_corruption_cases),
    ("check multiple scan issues", test_check_reports_multiple_scan_issues),
    ("check repair writes clean archive", test_check_repair_writes_clean_archive),
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
