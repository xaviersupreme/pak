# pak

Small file packer CLI written in C.

## Build

```sh
make
```

On windows without `make`:

```powershell
.\make.ps1
```

## Commands

```sh
pak make assets.pak image.png sound.wav config.txt
pak make --compress --overwrite assets.pak big.dat
pak make --paths assets.pak assets/image.png assets/sound.wav
pak make --compress assets.pak big.dat

pak list assets.pak
pak list --long assets.pak

pak extract assets.pak
pak extract -C out assets.pak
pak extract -C out assets.pak config.txt image.png
pak extract --overwrite -C out assets.pak
pak extract --skip-existing -C out assets.pak

pak cat assets.pak config.txt
pak info assets.pak
pak verify assets.pak
pak test assets.pak
```

## Flags

* `--v`, `-v`, `--verbose`: print progress while packing or extracting.
* `--paths`: store each files relative path, rather than just its base name.
* `--compress`: use the built in RLE compression when it reduces the size of an entry.
* `--long`: include stored size, method, and CRC32 when using `list`.
* `-C dir`: extract files into `dir`.
* `--overwrite`: overwrite files that already exist during extraction.
* `--skip-existing`: keep existing files and skip extracting those entries.
* `--`: stop option parsing so filenames can start with `-`.

By default extraction will not overwrite existing files.

`extract` accepts optional file names. When names are provided, only matching archive entries are extracted.
Options can appear before or after the command and between positional arguments.
