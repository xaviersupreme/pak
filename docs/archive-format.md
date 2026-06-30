# archive format

pak archives are simple on purpose. A file starts with one archive header, then each entry is written one after another.

There is no central directory. Listing an archive means walking entries from the start.

## what is stored

Each entry stores enough information to name, check, and unpack one file:

* archive name
* original size
* stored size
* storage method
* CRC32 of the original bytes
* stored bytes

The stored bytes are either copied as-is or compressed.

## file layout

A pak file starts with:

| field | size |
| --- | ---: |
| magic `PAK2` | 4 bytes |
| entry count | 4 bytes |

After that, entries are written back to back:

| field | size |
| --- | ---: |
| name length | 4 bytes |
| flags | 4 bytes |
| original size | 8 bytes |
| stored size | 8 bytes |
| CRC32 | 4 bytes |
| name bytes | name length |
| data bytes | stored size |

All integers are little endian.

## methods

The method comes from the entry flags.

| flags | method | data bytes |
| ---: | --- | --- |
| `0` | store | original file bytes |
| `1` | rle | pak run-length encoded bytes |
| `2` | deflate | miniz deflate bytes |

Only one method flag is valid for an entry.

`store` is used when compression is disabled or when compression does not make the entry smaller.

`rle` is a small run-length encoder used by pak. It is simple and only wins on some repeated data.

`deflate` uses miniz. With `--compress`, pak may skip deflate for large files when a sample shows the savings are too weak. `--no-smart-compress` disables that early skip.

## names

Archive names use `/` as the separator.

Valid names are relative paths inside the archive. They cannot:

* be empty
* start with `/` or `\`
* contain `\`
* contain a drive letter like `C:`
* contain empty path parts
* contain `.` or `..` path parts

These rules keep extraction from writing outside the output directory.

When `--paths` is not used, normal file inputs are stored by base name. Directory inputs keep paths relative to the directory that was packed.

## crc

CRC32 is calculated from the original uncompressed bytes.

For `store`, the original bytes and stored bytes are the same.

For `rle` and `deflate`, pak decompresses the entry and then checks the CRC against the decompressed bytes.

`pak check` uses the CRC to catch damaged data that still has a readable header.

## repair

`pak check` can write a repaired archive when damage is found and the user agrees.

Repair is best effort. It can:

* copy clean entries
* skip entries with broken headers
* recover readable bytes from damaged entries
* pad partially recovered entries so the repaired archive is valid
* resync to a later valid entry when enough structure remains

It cannot recreate bytes that are gone. If compressed data is destroyed in the middle, pak may only recover the part that still decompresses cleanly.
