# pak

`pak` bundles loose files and folders into one archive.

Use it for asset packs, tools, configs, test fixtures, and simple project data.

## build

```sh
make
```

Windows fallback:

```powershell
.\make.ps1
```

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
pak list [--long] <archive.pak>
pak extract [options] <archive.pak> [files...]
pak unpack [options] <archive.pak> [files...]
pak cat <archive.pak> <file>
pak info <archive.pak>
pak verify <archive.pak>
pak test <archive.pak>
```

`unpack` is the same command as `extract`.

## examples

```sh
pak make --compress assets sprites.png theme.wav config.txt
pak make --compress assets assets/
pak make --paths game assets/sprites/player.png assets/audio/jump.wav
pak list --long assets.pak
pak unpack assets.pak config.txt -C out
pak unpack --overwrite assets.pak -C out
pak cat assets.pak config.txt
pak test assets.pak
```

Use `--` when a filename starts with `-`:

```sh
pak make weird -- -dash.txt
```

## options

`--compress`
: Use deflate through miniz when it makes an entry smaller. RLE is still tried and used when it wins.

`--paths`
: Store relative paths instead of only base names.

`--long`
: Show readable sizes, stored size, ratio, method, and CRC32 in `list`.

`-C dir`
: Unpack into `dir`.

`--overwrite`
: Replace existing files while unpacking.

`--skip-existing`
: Leave existing files alone.

`--`
: Stop option parsing.

By default, unpacking refuses to overwrite files.

`extract` and `unpack` accept optional file names. When names are provided, only matching entries are unpacked.

Options can appear before or after the command and between positional arguments.

When a directory is passed to `make`, pak walks it recursively and stores paths for the files inside it.

## format

Current archives are `PAK2`.

Each entry stores:

* name
* original size
* stored size
* method: `store`, `rle`, or `deflate`
* CRC32
* raw entry bytes

`pak` can still read older `PAK1` archives.
