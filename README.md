# pak

`pak` bundles loose files and folders into one archive.

Use it for asset packs, tools, configs, test fixtures, and simple project data.

<p align="left">
  <img src="https://img.shields.io/github/v/release/xaviersupreme/pak" />
  <img src="https://img.shields.io/github/license/xaviersupreme/pak" />
  <img src="https://img.shields.io/github/stars/xaviersupreme/pak" />
  <img src="https://img.shields.io/github/issues/xaviersupreme/pak" />
  <img src="https://img.shields.io/github/last-commit/xaviersupreme/pak" />
  <img src="https://img.shields.io/github/languages/top/xaviersupreme/pak" />
</p>


## build

```sh
make
```

Windows fallback w/o make:

```powershell
.\make.ps1
```

Development notes are in [docs/development.md](docs/development.md).

<!-- ## releases

Builds are on the GitHub Releases page. -->

## quick start

```sh
pak make assets image.png sound.wav config.txt
pak list assets.pak
pak unpack assets.pak -C out
```

`make` appends `.pak` when the archive name does not already end in `.pak`.

## commands

```sh
pak make [options] <archive> <files...>
pak make [options] <archive> <dirs...>
pak update [options] <archive> <files...>
pak list [--long] [--full-name] <archive.pak> [files...|patterns...]
pak unpack|extract [options] <archive.pak> [files...|patterns...]
pak cat <archive.pak> <file|pattern>
pak info <archive.pak>
pak delete <archive.pak> <files...|patterns...>
pak rename <archive.pak> <old> <new>
pak repack [options] <archive.pak> [files...|patterns...]
pak check <archive.pak>
```

## examples

```sh
pak make --compress assets sprites.png theme.wav config.txt
pak make --compress --level 9 assets assets/
pak make --paths game assets/sprites/player.png assets/audio/jump.wav
pak make assets . --exclude "*.o"

pak update assets.pak config.txt
pak update assets.pak "*.c"
pak update assets.pak "**/*.c"

pak list --long assets.pak
pak list --long --full-name assets.pak
pak list assets.pak "*.png"

pak unpack assets.pak "*.png" -C out
pak unpack assets.pak config.txt -C out
pak unpack --overwrite assets.pak -C out

pak repack assets.pak
pak repack assets.pak "*.json" --level 9
pak repack assets.pak "*.wav" --store

pak cat assets.pak config.txt
pak delete assets.pak "*.tmp"
pak rename assets.pak config.txt config/default.txt
pak check assets.pak
```

`check` validates archive data. If it finds damage in the pak file, it offers to write `<name>.repaired.pak`. Clean entries are copied while damaged payloads with readable headers are saved with recovered bytes plus padding, and also entries with broken headers are skipped unless pak can resync to a later valid entry.

Use `--` when a filename starts with `-`:

```sh
pak make weird -- -dash.txt
```

## options

Flags are command scoped. They can appear before or after the command, but pak rejects flags that do not belong to that command.

### compression

`--compress`
: Use deflate through miniz when it makes an entry smaller. Large files need a stronger sample win before pak spends time compressing them. RLE is still tried when compression is worth checking. Works with `make`, `update`, and `repack`. `repack` uses this behavior by default.

`--no-smart-compress`
: Enable compression without file type or sample checks. pak still stores an entry if the final compressed data is not smaller.

`--level N`
: Set deflate level from `0` to `10`. This also turns on compression. Short forms `-0` through `-9` work too.

### make/update

Input paths may use `*`, `?`, and recursive `**`. Quote them when you want pak to expand the match itself, which is useful on Windows. `*.c` matches the current directory. `**/*.c` matches subdirectories too.

`--paths`
: Store relative paths instead of only base names.

`--exclude pattern`
: Skip matching files while packing. `*` and `?` are supported.

`--no-pakignore`
: Do not read `.pakignore`.

### repack

`--store`
: Rewrite matching entries without compression.

### list

`--long`
: Show readable sizes, stored size, ratio, method, and CRC32 in `list`. Long names are shortened by default.

`--full-name`
: Show full entry names in `list --long`. This also enables long output.

### extract/unpack

`-C dir`
: Unpack into `dir`.

`--overwrite`
: Replace existing files while unpacking.

`--skip-existing`
: Leave existing files alone.

### global

`--`
: Stop option parsing.

<br>

By default, unpacking refuses to overwrite files.

`list`, `extract`, `unpack`, and `delete` accept optional file names or wildcard patterns. `cat` can use a wildcard pattern when it matches exactly one file. Quote wildcard patterns in shells.

When a directory is passed to `make`, pak walks it recursively and stores paths for the files inside it. Files are written in archive name order, so the same inputs make the same archive bytes.

## .pakignore

`make` and `update` read `.pakignore` from the current directory. Blank lines are ignored. Lines that start with `#` are comments. They're just gitignore in disguise.

```gitignore
*.o
build/
cache/*.tmp
```

A pattern ending in `/` skips a folder. A pattern without `/` matches the file name.

## format

Current archives are `PAK2`.

The archive layout is described in [docs/archive-format.md](docs/archive-format.md).
