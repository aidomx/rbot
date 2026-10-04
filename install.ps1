# install.ps1 — pasang rbot untuk Windows.
#
# Pemakaian:
#   irm https://raw.githubusercontent.com/aidomx/rbot/main/install.ps1 | iex
#   .\install.ps1 --dev
#   .\install.ps1 --version v0.2.0
#
# Mode default mengunduh binary rbot Windows dari GitHub Releases.
# Mode --dev melakukan bootstrap build dari source dengan compiler C lokal.
#
# Compiler yang didukung:
#   clang
#   gcc
#   cl (MSVC Developer Command Prompt)

$ErrorActionPreference = "Stop"

$GitHubUser = "aidomx"
$RepoName   = "rbot"
$Branch     = "main"
$Version    = "v0.1.8"   # fallback; ditimpa oleh .rbot-version saat --dev

# ---------------------------------------------------------------------------
# Helper umum
# ---------------------------------------------------------------------------

function Read-Version([string]$Root) {
    $file = Join-Path $Root ".rbot-version"
    if (Test-Path $file) {
        $v = (Get-Content $file -TotalCount 1).Trim()
        if ($v) { return $v }
    }
    return $null
}

function Find-Compiler {
    # Hormati CC (seperti install.sh ${CC:-cc}) — dipakai CI untuk memilih
    # toolchain secara eksplisit (mis. CC=cl untuk job MSVC).
    if ($env:CC) {
        if (Get-Command $env:CC -ErrorAction SilentlyContinue) {
            return $env:CC
        }
        throw "compiler C '$env:CC' (dari CC) tidak ditemukan di PATH. " +
              "Jika ini job MSVC, jalankan ilammy/msvc-dev-cmd sebelum step ini."
    }
    foreach ($name in @("clang", "gcc", "cl")) {
        if (Get-Command $name -ErrorAction SilentlyContinue) { return $name }
    }
    return $null
}

function Install-Binary([string]$Binary) {
    $InstallDir  = Join-Path $env:LOCALAPPDATA "rbot"
    $InstallPath = Join-Path $InstallDir "rbot.exe"

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Copy-Item -Force $Binary $InstallPath

    # Tambahkan ke User PATH jika belum ada.
    $UserPath  = [Environment]::GetEnvironmentVariable("Path", "User")
    $PathParts = @()
    if ($UserPath) { $PathParts = $UserPath -split ";" | Where-Object { $_ } }

    if ($PathParts -notcontains $InstallDir) {
        $NewPath = (($PathParts + $InstallDir) -join ";")
        [Environment]::SetEnvironmentVariable("Path", $NewPath, "User")
    }

    Write-Host "> Installed  : $InstallPath"

    if (Test-Path $InstallPath) {
        & $InstallPath version 2>$null
    }

    Write-Host "> Note       : buka terminal baru agar PATH diperbarui."
}

# ---------------------------------------------------------------------------
# Parsing argumen
# ---------------------------------------------------------------------------

$Dev        = $false
$VersionTag = $null

for ($i = 0; $i -lt $args.Count; $i++) {
    switch ($args[$i]) {
        "--dev" {
            $Dev = $true
        }
        "--version" {
            if (($i + 1) -lt $args.Count) {
                $VersionTag = $args[$i + 1]
                $i++
            } else {
                throw "--version memerlukan argumen tag (mis. --version v0.2.0)"
            }
        }
        default {
            # abaikan argumen tak dikenal
        }
    }
}

$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }

$LocalSource = (
    (Test-Path (Join-Path $ScriptDir "src\main.c")) -and
    (Test-Path (Join-Path $ScriptDir "include\rbot.h"))
)

# ---------------------------------------------------------------------------
# Mode --dev: bootstrap build dari source
# ---------------------------------------------------------------------------

if ($Dev) {
    $TempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("rbot-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $TempDir | Out-Null

    # Output final diletakkan di luar $TempDir supaya tidak ikut terhapus di finally.
    $StagingDir = Join-Path ([System.IO.Path]::GetTempPath()) ("rbot-build-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $StagingDir | Out-Null

    try {
        if ($LocalSource) {
            $SrcDir = $ScriptDir
        } else {
            $SourceUrl = "https://github.com/$GitHubUser/$RepoName/archive/refs/heads/$Branch.zip"
            $Archive   = Join-Path $TempDir "rbot.zip"

            Write-Host "> Downloading source: $SourceUrl"
            Invoke-WebRequest -Uri $SourceUrl -OutFile $Archive

            Expand-Archive -Path $Archive -DestinationPath $TempDir -Force
            $SrcDir = Get-ChildItem $TempDir -Directory |
                Where-Object { $_.Name -ne "build" } |
                Select-Object -First 1 -ExpandProperty FullName
        }

        if (-not $SrcDir -or -not (Test-Path (Join-Path $SrcDir "src\main.c"))) {
            throw "source tree tidak valid (src\main.c tidak ditemukan di '$SrcDir')"
        }

        $found = Read-Version $SrcDir
        if ($found) { $Version = $found }

        $Compiler = Find-Compiler
        if (-not $Compiler) {
            throw "compiler C tidak ditemukan; pasang clang, gcc, atau gunakan MSVC Developer Command Prompt"
        }

        Write-Host "> Bootstrap  : rbot $Version"
        Write-Host "> Compiler   : $Compiler"

        $ObjDir = Join-Path $TempDir "obj"
        New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

        $Sources = Get-ChildItem (Join-Path $SrcDir "src") -Filter "*.c" -Recurse
        if ($Sources.Count -eq 0) {
            throw "tidak ada file sumber (.c) ditemukan di $(Join-Path $SrcDir 'src')"
        }

        $IncludeDir = Join-Path $SrcDir "include"
        $SrcInner   = Join-Path $SrcDir "src"
        $Total      = $Sources.Count
        $Done       = 0
        $Objects    = @()

        foreach ($Source in $Sources) {
            $Relative    = $Source.FullName.Substring($SrcInner.Length).TrimStart("\")
            $ObjRelative = [System.IO.Path]::ChangeExtension($Relative, ".o")
            $Obj         = Join-Path $ObjDir $ObjRelative
            $ObjParent   = Split-Path $Obj -Parent

            New-Item -ItemType Directory -Force -Path $ObjParent | Out-Null

           $global:LASTEXITCODE = 0
            if ($Compiler -eq "cl") {
                $ClArgs = @(
                    "/nologo", "/std:c11", "/O2", "/W4",
                    "/DRBOT_VERSION_EMBEDDED=`"$Version`"",
                    "/I$IncludeDir",
                    "/I$SrcInner",
                    "/c", $Source.FullName,
                    "/Fo$Obj"
                )
                Write-Host ">> cl @ $($Source.Name)"
                & cl @ClArgs
            } else {
                $CcArgs = @(
                    "-std=gnu11", "-O2", "-Wall", "-Wextra",
                    "-DRBOT_VERSION_EMBEDDED=`"$Version`"",
                    "-I$IncludeDir",
                    "-I$SrcInner",
                    "-c", $Source.FullName,
                    "-o", $Obj
                )
                Write-Host ">> $Compiler @ $($Source.Name)"
                & $Compiler @CcArgs
            }

            $exitCode = $LASTEXITCODE
            if ($exitCode -ne 0) {
                throw "gagal mengompilasi $($Source.FullName) (exit $exitCode)"
            } 

            $Objects += $Obj
            $Done++
            $Percent = [int](($Done * 90) / $Total)
            Write-Progress -Activity "Building rbot" -Status "$Percent%" -PercentComplete $Percent
        }

        $Output = Join-Path $StagingDir "rbot.exe"

        $global:LASTEXITCODE = 0
        if ($Compiler -eq "cl") {
            # shell32: fsRemoveTree (SHFileOperationA) — sama dengan
            # target_link_libraries(literal shell32) di CMakeLists lama.
            $LinkArgs = @($Objects) + @(
                "shell32.lib",
                "/nologo",
                "/SUBSYSTEM:CONSOLE",
                "/OUT:$Output"
            )
            $output = & link @LinkArgs 2>&1
        } else {
            $LinkArgs = @($Objects) + @("-lshell32", "-o", $Output)
            $output = & $Compiler @LinkArgs 2>&1
        }

        if ($LASTEXITCODE -ne 0) {
            Write-Host ($output | Out-String)
            throw "gagal melakukan linking rbot (exit $LASTEXITCODE)"
        }

        Write-Progress -Activity "Building rbot" -Completed
        Write-Host "> Built      : $Output"

        # Install dari staging (di luar TempDir) supaya tidak dihapus finally.
        Install-Binary $Output
    }
    finally {
        Remove-Item -Recurse -Force $TempDir    -ErrorAction SilentlyContinue
        Remove-Item -Recurse -Force $StagingDir -ErrorAction SilentlyContinue
    }

    exit 0
}

# ---------------------------------------------------------------------------
# Mode default: unduh binary dari GitHub Releases
# ---------------------------------------------------------------------------

function Get-ReleaseInfo([string]$Tag) {
    $api = if ($Tag) {
        "https://api.github.com/repos/$GitHubUser/$RepoName/releases/tags/$Tag"
    } else {
        "https://api.github.com/repos/$GitHubUser/$RepoName/releases/latest"
    }
    $headers = @{
        "User-Agent" = "rbot-installer"
        "Accept"     = "application/vnd.github+json"
    }
    try {
        return Invoke-RestMethod -Uri $api -Headers $headers
    } catch {
        throw "gagal mengambil release dari GitHub API ($api): $($_.Exception.Message)"
    }
}

function Get-WindowsAssetUrl([string]$Tag) {
    $release = Get-ReleaseInfo $Tag
    $asset = $release.assets |
        Where-Object { $_.name -eq "rbot-windows-x64.exe" } |
        Select-Object -First 1

    if (-not $asset) {
        $names = ($release.assets | ForEach-Object { $_.name }) -join ", "
        throw "asset 'rbot-windows-x64.exe' tidak ada di release $($release.tag_name). Asset tersedia: $names"
    }

    Write-Host "> Release    : $($release.tag_name)"
    return $asset.browser_download_url
}

$TempFile = Join-Path ([System.IO.Path]::GetTempPath()) ("rbot-" + [guid]::NewGuid().ToString("N") + ".exe")

try {
    $url = Get-WindowsAssetUrl $VersionTag
    Write-Host "> Downloading: $url"
    Invoke-WebRequest -Uri $url -OutFile $TempFile
    Install-Binary $TempFile
}
finally {
    Remove-Item -Force $TempFile -ErrorAction SilentlyContinue
}
