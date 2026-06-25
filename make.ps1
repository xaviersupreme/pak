$ErrorActionPreference = 'Stop'

$cc = 'cc'
if (Get-Command clang -ErrorAction SilentlyContinue) {
    $cc = 'clang'
}

& $cc -std=c11 -Wall -Wextra -pedantic -O2 `
    -Iinclude `
    src/main.c src/archive.c src/io.c src/log.c src/endian.c src/crc.c src/compress.c `
    -o pak.exe