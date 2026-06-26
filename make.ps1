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
    -Iinclude -Ivendor/miniz `
    src/main.c src/archive.c src/io.c src/paths.c src/log.c src/endian.c src/crc.c src/compress.c vendor/miniz/miniz.c vendor/miniz/miniz_tdef.c vendor/miniz/miniz_tinfl.c `
    -o pak.exe
