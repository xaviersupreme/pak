param(
    [ValidateSet('all', 'test')]
    [string]$Target = 'all'
)

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
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if ($Target -eq 'test') {
    $pythonCandidates = @(
        @{ Exe = 'py'; Args = @('-3') },
        @{ Exe = 'python'; Args = @() },
        @{ Exe = 'python3'; Args = @() }
    )
    $python = $null
    foreach ($candidate in $pythonCandidates) {
        if (Get-Command $candidate.Exe -ErrorAction SilentlyContinue) {
            $python = $candidate
            break
        }
    }

    if ($null -eq $python) {
        Write-Error 'no Python 3 found. install Python 3 to run tests.'
        exit 1
    }

    & $python.Exe @($python.Args + @('dev/tests/test_cli.py', '--pak', '.\pak.exe'))
    exit $LASTEXITCODE
}
