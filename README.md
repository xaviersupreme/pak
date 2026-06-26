# pak

`pak` bundles loose files into one simple archive.

It is meant for asset packs, small tools, configs, test fixtures, and anything else where a plain "put these files in one file" format is enough.

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
pak list [--long] <archive.pak>
pak extract [options] <archive.pak> [files...]
pak unpack [options] <archive.pak> [files...]
pak cat <archive.pak> <file>
pak info <archive.pak>
pak verify <archive.pak>
pak test <archive.pak>
```

`unpack` is the same as `extract`.

## examples

```sh
pak make --compress assets sprites.png theme.wav config.txt
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

| option | use |
| --- | --- |
| `--compress` | use built-in RLE when it makes an entry smaller |
| `--paths` | store relative paths instead of only base names |
| `--long` | show stored size, method, and CRC32 in `list` |
| `-C dir` | unpack into `dir` |
| `--overwrite` | replace existing files while unpacking |
| `--skip-existing` | leave existing files alone |
| `--` | stop option parsing |

By default, unpacking refuses to overwrite files.

`extract` and `unpack` accept optional file names. When names are provided, only matching entries are unpacked.

Options can appear before or after the command and between positional arguments.

## format

Current archives are `PAK2`.

Each entry stores:

- name
- uncompressed size
- stored size
- method: `store` or `rle`
- CRC32
- raw entry bytes

`pak` can still read older `PAK1` archives.
