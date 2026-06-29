$ErrorActionPreference = 'Stop'

$cc = $null
foreach ($candidate in @('clang', 'gcc', 'cc')) {
    if (Get-Command $candidate -ErrorAction SilentlyContinue) {
        $cc = $candidate
        break
    }
}

if ($null -eq $cc) {
    Write-Error 'no C compiler found. install clang, gcc, or cc.'
    exit 1
}

& $cc -std=c11 -Wall -Wextra -pedantic -O2 `
    -Iinclude -Ivendor/miniz -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES `
    src/cli/main.c src/cli/hints.c src/archive/core.c src/archive/check.c src/codec/compress.c src/codec/crc.c src/fs/io.c src/fs/paths.c src/fs/pattern.c src/output/log.c src/output/diag.c src/format/endian.c vendor/miniz/miniz.c vendor/miniz/miniz_tdef.c vendor/miniz/miniz_tinfl.c `
    -o pak.exe
