# development

This is for working on pak itself.

## tests

Run the CLI regression tests:

```sh
make test
```

PowerShell fallback:

```powershell
.\make.ps1 test
```

The test runner lives at `dev/tests/test_cli.py`. It creates temporary archives and covers the normal command flow: `make`, `update`, `list`, `cat`, `unpack`, `delete`, `rename`, `repack`, and `check`.

It also covers path cases that have broken before:

* directory packing
* wildcard inputs
* mixed file and directory inputs
* absolute file inputs
* smart compression and `--no-smart-compress`

## source install

For local source installs or package scripts:

```sh
make install PREFIX=/usr/local
make uninstall PREFIX=/usr/local
```

Package scripts can also use `DESTDIR`.

## fuzzing

Quick deterministic smoke test:

```sh
make fuzz
```

Seeded LLVM libFuzzer run:

```sh
make fuzz-run
```

Useful variables:

```sh
make fuzz FUZZ_ITERS=100000
make fuzz-run-libfuzzer FUZZ_CC=clang FUZZ_SANITIZERS=fuzzer,address
./dev/fuzz/bin/archive_fuzz dev/fuzz/corpus/archive -max_total_time=300
```

On Windows, `FUZZ_SANITIZERS=fuzzer` is usually enough. On Linux and macOS, use `fuzzer,address`.

Fuzz outputs are ignored under `dev/fuzz/bin/` and `dev/fuzz/corpus/`.
