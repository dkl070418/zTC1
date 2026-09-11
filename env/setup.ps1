<#
    Reproducible, fully project-local toolchain bootstrap for the TC1 firmware.

    Everything is downloaded from CN mirrors and unpacked INSIDE this repo:
        env\dl            downloaded archives
        env\python27      portable Python 2.7.18 (+ pip / setuptools / mico-cube)
        env\cache         pip + temp caches (never touches the system drive)
        env\home          HOME/USERPROFILE so mico-cube writes ~/.mico here
        mico-os\MiCoder   MXCHIP toolchain (makefiles hard-code this path)

    Re-runnable: existing pieces are skipped.
#>
[CmdletBinding()]
param(
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root    = Split-Path -Parent $PSScriptRoot
$EnvDir  = Join-Path $Root 'env'
$Dl      = Join-Path $EnvDir 'dl'
$Py      = Join-Path $EnvDir 'python27'
$Cache   = Join-Path $EnvDir 'cache'
$MiCoder = Join-Path $Root 'mico-os\MiCoder'

# keep every scratch location inside the project
$env:HOME        = Join-Path $EnvDir 'home'
$env:USERPROFILE = $env:HOME
$env:TMP         = Join-Path $Cache 'tmp'
$env:TEMP        = $env:TMP
foreach ($d in @($Dl, $env:HOME, $env:TMP, (Join-Path $Cache 'pip'), $Py)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

function Get-Remote($url, $out) {
    if ((Test-Path -LiteralPath $out) -and -not $Force) {
        Write-Host "  have $(Split-Path $out -Leaf)" -ForegroundColor DarkGray
        return
    }
    Write-Host "  get $(Split-Path $out -Leaf)" -ForegroundColor Cyan
    & curl.exe -L --fail --retry 8 --retry-delay 2 -C - -sS --no-progress-meter -o $out $url
    if ($LASTEXITCODE -ne 0) { throw "download failed ($LASTEXITCODE): $url" }
}

# ---------------------------------------------------------------- 1. MiCoder --
# mico-os/makefiles/micoder_host_cmd.mk hard-codes TOOLS_ROOT = mico-os/MiCoder,
# and mico-cube also looks there first, so no `mico config --global` is needed.
Write-Host '==> [1/4] MXCHIP MiCoder (toolchain + make + openocd)' -ForegroundColor Green
$micoderZip = Join-Path $Dl 'MiCoder_v1.3_Win64.zip'
Get-Remote 'http://firmware.mxchip.com/MiCoder_v1.3_Win32:64.zip' $micoderZip

if (-not (Test-Path -LiteralPath (Join-Path $MiCoder 'cmd\Win32\make.exe'))) {
    Write-Host '  extracting (~1 GB, this takes a minute)' -ForegroundColor Cyan
    Push-Location (Join-Path $Root 'mico-os')
    try {
        & tar.exe -xf $micoderZip
        if ($LASTEXITCODE -ne 0) { throw "MiCoder extract failed ($LASTEXITCODE)" }
        Remove-Item -LiteralPath (Join-Path (Get-Location) '__MACOSX') -Recurse -Force -ErrorAction SilentlyContinue
    } finally { Pop-Location }
}

# ------------------------------------------------------------- 2. Python 2.7 --
Write-Host '==> [2/4] portable Python 2.7.18 (npmmirror)' -ForegroundColor Green
$pyMsi = Join-Path $Dl 'python-2.7.18.amd64.msi'
Get-Remote 'https://registry.npmmirror.com/-/binary/python/2.7.18/python-2.7.18.amd64.msi' $pyMsi

if (-not (Test-Path -LiteralPath (Join-Path $Py 'python.exe'))) {
    # administrative install: unpacks the MSI payload, no registry / no admin rights
    $p = Start-Process msiexec.exe -ArgumentList '/a', "`"$pyMsi`"", '/qn', "TARGETDIR=$Py" `
                       -Wait -PassThru -NoNewWindow
    if ($p.ExitCode -ne 0) { throw "msiexec /a failed ($($p.ExitCode))" }
    Remove-Item -LiteralPath (Join-Path $Py 'python-2.7.18.amd64.msi') -Force -ErrorAction SilentlyContinue
}
& (Join-Path $Py 'python.exe') -c 'import sys; print sys.version'

# ------------------------------------------------- 3. pip / setuptools / mico --
Write-Host '==> [3/4] pip + setuptools + mico-cube (Tsinghua PyPI mirror)' -ForegroundColor Green
$pipWhl = Join-Path $Dl 'pip-20.3.4-py2.py3-none-any.whl'
$setWhl = Join-Path $Dl 'setuptools-44.0.0-py2.py3-none-any.whl'
$micoTgz = Join-Path $Dl 'mico-cube-1.0.0.tar.gz'
Get-Remote 'https://pypi.tuna.tsinghua.edu.cn/packages/27/79/8a850fe3496446ff0d584327ae44e7500daf6764ca1a382d2d02789accf7/pip-20.3.4-py2.py3-none-any.whl' $pipWhl
Get-Remote 'https://pypi.tuna.tsinghua.edu.cn/packages/f9/d3/955738b20d3832dfa3cd3d9b07e29a8162edb480bf988332f5e6e48ca444/setuptools-44.0.0-py2.py3-none-any.whl' $setWhl
Get-Remote 'https://pypi.tuna.tsinghua.edu.cn/packages/a8/4e/687a3ef2edabfd9fdd797747d6eeb2ddbe70e65c49fa4062268281a0ded4/mico-cube-1.0.0.tar.gz' $micoTgz

$site = Join-Path $Py 'Lib\site-packages'
foreach ($w in @($pipWhl, $setWhl)) {
    # pure-python wheels: unpack straight into site-packages, no build step needed
    & tar.exe -xf $w -C $site
    if ($LASTEXITCODE -ne 0) { throw "wheel extract failed: $w" }
}

$MicroExe = Join-Path $Py 'Scripts\mico.exe'
if (-not (Test-Path -LiteralPath $MicroExe) -or $Force) {
    & (Join-Path $Py 'python.exe') -m pip install --no-build-isolation --no-index `
        --find-links $Dl --cache-dir (Join-Path $Cache 'pip') $micoTgz
    if (-not (Test-Path -LiteralPath $MicroExe)) { throw 'mico.exe was not installed' }
}

# ------------------------------------------------------------- 4. sanity check --
Write-Host '==> [4/4] verify' -ForegroundColor Green
& $MicroExe --version
$gcc = Join-Path $MiCoder 'compiler\arm-none-eabi-5_4-2016q2-20160622\Win32\bin\arm-none-eabi-gcc.exe'
& $gcc --version | Select-Object -First 1

Write-Host "`ndone. build with:  .\env\build.ps1" -ForegroundColor Green
