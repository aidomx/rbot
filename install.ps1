# install.ps1 — pasang rbot untuk Windows.
#
# Pemakaian:
#   irm https://raw.githubusercontent.com/aidomx/rbot/main/install.ps1 | iex
#   .\install.ps1 --dev
#
# Mode default mengunduh binary rbot Windows yang sudah di-build.
# Mode --dev melakukan bootstrap build dari source dengan compiler C lokal.
#
# Compiler yang didukung:
#   clang
#   gcc
#   cl (MSVC Developer Command Prompt)

$ErrorActionPreference = "Stop"

$GitHubUser = "aidomx"
$RepoName = "rbot"
$Branch = "main"
$BinPathInRepo = "bin/rbot.exe"
$Version = "v0.1.8"

function Read-Version([string]$Root) {
    $file = Join-Path $Root ".rbot-version"
    if (Test-Path $file) {
        $v = (Get-Content $file -TotalCount 1).Trim()
        if ($v) {
            $script:Version = $v
        }
    }
}

function Find-Compiler {
    $candidates = @("clang", "gcc", "cl")
    foreach ($name in $candidates) {
        if (Get-Command $name -ErrorAction SilentlyContinue) {
            return $name
        }
    }
    return $null
}

function Install-Binary([string]$Binary) {
    $InstallDir = Join-Path $env:LOCALAPPDATA "rbot"
    $InstallPath = Join-Path $InstallDir "rbot.exe"

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Copy-Item -Force $Binary $InstallPath

    # Tambahkan ke User PATH jika belum ada.
    $UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $PathParts = @()
    if ($UserPath) {
        $PathParts = $UserPath -split ";" | Where-Object { $_ }
    }

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

$Dev = ($args.Count -gt 0 -and $args[0] -eq "--dev")

$ScriptDir = if ($PSScriptRoot) {
    $PSScriptRoot
} else {
    (Get-Location).Path
}

$LocalSource = (
    (Test-Path (Join-Path $ScriptDir "src\main.c")) -and
    (Test-Path (Join-Path $ScriptDir "include\rbot.h"))
)

if ($Dev) {
    $TempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("rbot-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $TempDir | Out-Null

    try {
        if ($LocalSource) {
            $SrcDir = $ScriptDir
        } else {
            $SourceUrl = "https://github.com/$GitHubUser/$RepoName/archive/refs/heads/$Branch.zip"
            $Archive = Join-Path $TempDir "rbot.zip"

            Write-Host "> Downloading source: $SourceUrl"
            Invoke-WebRequest -Uri $SourceUrl -OutFile $Archive

            Expand-Archive -Path $Archive -DestinationPath $TempDir -Force
            $SrcDir = Get-ChildItem $TempDir -Directory |
                Where-Object { $_.Name -ne "build" } |
                Select-Object -First 1 -ExpandProperty FullName
        }

        if (-not $SrcDir -or -not (Test-Path (Join-Path $SrcDir "src\main.c"))) {
            throw "source tree tidak valid"
        }

        Read-Version $SrcDir

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
            throw "tidak ada file sumber (.c) ditemukan"
        }

        $Total = $Sources.Count
        $Done = 0
        $Objects = @()

        foreach ($Source in $Sources) {
            $Relative = $Source.FullName.Substring((Join-Path $SrcDir "src").Length).TrimStart("\")
            $ObjRelative = [System.IO.Path]::ChangeExtension($Relative, ".o")
            $Obj = Join-Path $ObjDir $ObjRelative
            $ObjParent = Split-Path $Obj -Parent

            New-Item -ItemType Directory -Force -Path $ObjParent | Out-Null

            if ($Compiler -eq "cl") {
                & cl /nologo /std:c11 /O2 /W4 `
                    "/DRBOT_VERSION_EMBEDDED=`"$Version`"" `
                    "/I$(Join-Path $SrcDir 'include')" `
                    "/I$(Join-Path $SrcDir 'src')" `
                    /c $Source.FullName "/Fo$Obj"
            } else {
                & $Compiler -std=gnu11 -O2 -Wall -Wextra `
                    "-DRBOT_VERSION_EMBEDDED=`"$Version`"" `
                    "-I$(Join-Path $SrcDir 'include')" `
                    "-I$(Join-Path $SrcDir 'src')" `
                    -c $Source.FullName -o $Obj
            }

            if ($LASTEXITCODE -ne 0) {
                throw "gagal mengompilasi $($Source.FullName)"
            }

            $Objects += $Obj
            $Done++
            $Percent = [int](($Done * 90) / $Total)
            Write-Progress -Activity "Building rbot" -Status "$Percent%" -PercentComplete $Percent
        }

        $OutDir = Join-Path $SrcDir "build\bin"
        New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
        $Output = Join-Path $OutDir "rbot.exe"

        if ($Compiler -eq "cl") {
            & link $Objects "/OUT:$Output"
        } else {
            & $Compiler $Objects -o $Output
        }

        if ($LASTEXITCODE -ne 0) {
            throw "gagal melakukan linking rbot"
        }

        Write-Progress -Activity "Building rbot" -Completed
        Write-Host "> Built & Saved: $Output"

        if (Test-Path $Output) {
            & $Output version 2>$null
        }
    }
    finally {
        Remove-Item -Recurse -Force $TempDir -ErrorAction SilentlyContinue
    }

    exit 0
}

$TempFile = Join-Path ([System.IO.Path]::GetTempPath()) ("rbot-" + [guid]::NewGuid().ToString("N") + ".exe")
$RawUrl = "https://raw.githubusercontent.com/$GitHubUser/$RepoName/$Branch/$BinPathInRepo"

try {
    Write-Host "> Downloading: $RawUrl"
    Invoke-WebRequest -Uri $RawUrl -OutFile $TempFile
    Install-Binary $TempFile
}
finally {
    Remove-Item -Force $TempFile -ErrorAction SilentlyContinue
}
