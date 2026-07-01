param(
    [string]$Version = "latest",
    [string]$InstallDir = "$env:LOCALAPPDATA\Programs\pak",
    [switch]$Setup
)

$ErrorActionPreference = "Stop"

$repo = "xaviersupreme/pak"
$arch = if ([Environment]::Is64BitOperatingSystem) { "x64" } else { "x86" }
$asset = if ($Setup) { "pak-windows-$arch-setup.exe" } else { "pak-windows-$arch.zip" }
$noColor = [string]::IsNullOrEmpty($env:NO_COLOR) -eq $false

function Write-Step($Message) {
    if ($noColor) {
        Write-Host "==> $Message"
    } else {
        Write-Host "==> " -ForegroundColor Cyan -NoNewline
        Write-Host $Message
    }
}

function Write-Ok($Message) {
    if ($noColor) {
        Write-Host "ok: $Message"
    } else {
        Write-Host "ok: " -ForegroundColor Green -NoNewline
        Write-Host $Message
    }
}

function Write-Warn($Message) {
    if ($noColor) {
        Write-Host "warn: $Message"
    } else {
        Write-Host "warn: " -ForegroundColor Yellow -NoNewline
        Write-Host $Message
    }
}

function Get-PakVersion($Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return $null
    }

    try {
        $output = & $Path --version 2>$null
        if ($LASTEXITCODE -eq 0 -and $output -match '^pak\s+(.+)$') {
            return $Matches[1]
        }
    } catch {
        return $null
    }

    return $null
}

function Get-LatestVersion {
    Write-Step "checking latest release"
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/latest"
    return $release.tag_name
}

function Get-InstalledSetupVersion {
    $key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\pak"
    if (-not (Test-Path $key)) {
        return $null
    }

    return (Get-ItemProperty $key).DisplayVersion
}

function Get-CommandPath($Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $cmd) {
        return $null
    }
    return $cmd.Source
}

$targetVersion = if ($Version -eq "latest") { Get-LatestVersion } else { $Version }
if ([string]::IsNullOrWhiteSpace($targetVersion)) {
    throw "could not resolve target version"
}

$target = Join-Path $InstallDir "pak.exe"
if ($Setup) {
    $installedVersion = Get-InstalledSetupVersion
    if ($installedVersion) {
        if ($installedVersion -eq $targetVersion) {
            Write-Ok "pak $installedVersion is already installed"
            exit 0
        }
        Write-Warn "pak $installedVersion is installed; updating to $targetVersion"
    } else {
        Write-Step "pak is not installed; installing $targetVersion"
    }
} elseif (Test-Path -LiteralPath $target) {
    $installedVersion = Get-PakVersion $target
    if ($installedVersion) {
        if ($installedVersion -eq $targetVersion) {
            Write-Ok "pak $installedVersion is already installed at $target"
            exit 0
        }
        Write-Warn "pak $installedVersion is installed at $target; updating to $targetVersion"
    } else {
        Write-Warn "pak is installed at $target, but its version is unknown; updating to $targetVersion"
    }
} else {
    $active = Get-CommandPath "pak"
    if ($active) {
        $activeVersion = Get-PakVersion $active
        if ($activeVersion) {
            Write-Warn "pak $activeVersion is installed at $active; installing $targetVersion to $target"
        } else {
            Write-Warn "pak is installed at $active, but its version is unknown; installing $targetVersion to $target"
        }
    } else {
        Write-Step "pak is not installed; installing $targetVersion"
    }
}

$url = "https://github.com/$repo/releases/download/$targetVersion/$asset"
$tmp = Join-Path $env:TEMP "pak-install-$PID"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

try {
    $download = Join-Path $tmp $asset
    Write-Step "downloading $asset"
    Invoke-WebRequest -Uri $url -OutFile $download

    if ($Setup) {
        Write-Step "starting installer"
        Start-Process -FilePath $download -Wait
        $installedVersion = Get-InstalledSetupVersion
        if ($installedVersion) {
            Write-Ok "installed pak $installedVersion"
        } else {
            Write-Ok "installer finished"
        }
        exit
    }

    Write-Step "installing pak to $InstallDir"
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Expand-Archive -Path $download -DestinationPath $tmp -Force
    Copy-Item -Path (Join-Path $tmp "pak.exe") -Destination $target -Force

    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $pathParts = @()
    if ($userPath) {
        $pathParts = $userPath -split ";" | Where-Object { $_ -ne "" }
    }
    if ($pathParts -notcontains $InstallDir) {
        $pathParts += $InstallDir
        [Environment]::SetEnvironmentVariable("Path", ($pathParts -join ";"), "User")
        Write-Warn "open a new terminal so PATH updates"
    }

    $installedVersion = Get-PakVersion $target
    if ($installedVersion) {
        Write-Ok "installed pak $installedVersion"
    } else {
        Write-Ok "installed pak"
    }

    $active = Get-CommandPath "pak"
    if ($active -and ([IO.Path]::GetFullPath($active) -ne [IO.Path]::GetFullPath($target))) {
        Write-Warn "current PATH finds $active before $target"
    }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
