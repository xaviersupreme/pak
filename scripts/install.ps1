param(
    [string]$Version = "latest",
    [string]$InstallDir = "$env:LOCALAPPDATA\Programs\pak",
    [switch]$Setup
)

$ErrorActionPreference = "Stop"

$repo = "xaviersupreme/pak"
$arch = if ([Environment]::Is64BitOperatingSystem) { "x64" } else { "x86" }
$asset = if ($Setup) { "pak-windows-$arch-setup.exe" } else { "pak-windows-$arch.zip" }

if ($Version -eq "latest") {
    $url = "https://github.com/$repo/releases/latest/download/$asset"
} else {
    $url = "https://github.com/$repo/releases/download/$Version/$asset"
}

$tmp = Join-Path $env:TEMP "pak-install-$PID"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

try {
    $download = Join-Path $tmp $asset
    Write-Host "==> downloading $asset"
    Invoke-WebRequest -Uri $url -OutFile $download

    if ($Setup) {
        Write-Host "==> starting installer"
        Start-Process -FilePath $download -Wait
        exit
    }

    Write-Host "==> installing pak to $InstallDir"
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Expand-Archive -Path $download -DestinationPath $tmp -Force
    Copy-Item -Path (Join-Path $tmp "pak.exe") -Destination (Join-Path $InstallDir "pak.exe") -Force

    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if (($userPath -split ";") -notcontains $InstallDir) {
        [Environment]::SetEnvironmentVariable("Path", "$userPath;$InstallDir", "User")
        Write-Host "hint: open a new terminal so PATH updates"
    }

    Write-Host "==> installed pak"
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
