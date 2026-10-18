# ZombieKing P1：以前台方式启动本地 nakama.exe（日志进控制台，Ctrl+C 停止）
$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$NakamaDir = Join-Path $RepoRoot "nakama"
$LocalYml  = Join-Path $NakamaDir "Data\local.yml"
$Dist      = Join-Path $NakamaDir "modules\dist\index.js"
$EnvFile   = Join-Path $ScriptDir "dev.env"

if (-not (Test-Path $LocalYml)) { throw "local.yml 不存在，请先运行 init-db.ps1" }
if (-not (Test-Path $Dist))     { throw "modules\dist\index.js 不存在，请先运行 build-modules.ps1" }

# 服务 Token 等注入进程环境变量（供 RPC 通过 ctx.environment 读取）
if (Test-Path $EnvFile) {
    Get-Content $EnvFile | ForEach-Object {
        if ($_ -match '^\s*([A-Z0-9_]+)=(.*)$') { Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2] }
    }
}

Set-Location $NakamaDir
Write-Host "启动 nakama.exe (config: Data\local.yml) ..."
& (Join-Path $NakamaDir "nakama.exe") --config (Join-Path $NakamaDir "Data\local.yml")
