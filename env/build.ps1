<#
    One-shot, fully project-local build wrapper for the TC1 firmware.

    Everything (toolchain, python, caches, temp files, mico config) stays inside
    this repository folder - nothing is written to the system drive.

    Usage:
        .\env\build.ps1                 # full build (regenerate web assets + mico make total)
        .\env\build.ps1 -NoWeb          # skip web_data.c regeneration
        .\env\build.ps1 -Args "download run"
#>
[CmdletBinding()]
param(
    [switch]$NoWeb,
    [string]$Args = 'total'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root    = Split-Path -Parent $PSScriptRoot
$EnvDir  = Join-Path $Root 'env'
$Py      = Join-Path $EnvDir 'python27\python.exe'
$MicroEx = Join-Path $EnvDir 'python27\Scripts\mico.exe'
$MiCoder = Join-Path $Root 'mico-os\MiCoder'

foreach ($p in @($Py, $MicroEx, $MiCoder)) {
    if (-not (Test-Path -LiteralPath $p)) {
        throw "missing build prerequisite: $p  (run env\setup.ps1 first)"
    }
}

# ---- keep every scratch / cache location inside the project -----------------
$env:HOME             = Join-Path $EnvDir 'home'
$env:USERPROFILE      = Join-Path $EnvDir 'home'
$env:TMP              = Join-Path $EnvDir 'cache\tmp'
$env:TEMP             = Join-Path $EnvDir 'cache\tmp'
$env:PIP_CACHE_DIR    = Join-Path $EnvDir 'cache\pip'
$env:PYTHONIOENCODING = 'gbk'
New-Item -ItemType Directory -Force -Path $env:HOME, $env:TMP, $env:PIP_CACHE_DIR | Out-Null

$env:Path = "$(Join-Path $EnvDir 'python27');$(Join-Path $EnvDir 'python27\Scripts');$(Join-Path $MiCoder 'cmd\Win32');" + $env:Path

Push-Location $Root
try {
    $LASTEXITCODE = 0

    if (-not $NoWeb) {
        Write-Host '==> regenerating web_data.c from TC1\http_server\web' -ForegroundColor Cyan
        $webDir = Join-Path $Root 'TC1\http_server'
        $outFile = Join-Path $webDir 'web_data.c'

        # 必须用 Start-Process 的 -RedirectStandardOutput：
        #   * PowerShell 自带的 '>' 会写成 UTF-16LE（带 BOM），C 编译器直接报错
        #   * cmd.exe /c "... > ..." 的引号经 PowerShell 传参会被吃掉，
        #     表现为 exit=0 但根本没生成文件 —— 静默失败，最坑
        # -RedirectStandardOutput 直接把子进程 stdout 的原始字节落盘，两者都规避。
        if (Test-Path -LiteralPath $outFile) { Remove-Item -LiteralPath $outFile -Force }

        $p = Start-Process -FilePath $Py -ArgumentList 'test.py' -WorkingDirectory $webDir `
                           -RedirectStandardOutput $outFile -RedirectStandardError (Join-Path $EnvDir 'cache\testpy.err') `
                           -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw "test.py failed (exit=$($p.ExitCode))" }

        # 生成后校验，杜绝再次静默产出空文件/旧文件
        if (-not (Test-Path -LiteralPath $outFile)) { throw "test.py produced no web_data.c" }
        if ((Get-Item -LiteralPath $outFile).Length -lt 100000) { throw "web_data.c suspiciously small" }
        if ((Get-Item -LiteralPath $outFile).LastWriteTime -lt (Get-Item -LiteralPath (Join-Path $webDir 'web\index.html')).LastWriteTime) {
            throw "web_data.c is older than web\index.html - regeneration did not take effect"
        }
        Write-Host ("    web_data.c regenerated: {0:N0} bytes" -f (Get-Item -LiteralPath $outFile).Length) -ForegroundColor DarkGray
    }

    Write-Host "==> mico make TC1@MK3031@moc $Args" -ForegroundColor Cyan
    & $MicroEx make 'TC1@MK3031@moc' @($Args -split ' ')
    if ($LASTEXITCODE -ne 0) { throw "mico make failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
