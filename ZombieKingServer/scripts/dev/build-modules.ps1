# ZombieKing P1：构建 Nakama TS 模块（npm install + tsc 类型检查 + esbuild 打包）
$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$Modules   = Join-Path $RepoRoot "nakama\modules"

Set-Location $Modules

if (-not (Test-Path (Join-Path $Modules "node_modules"))) {
    Write-Host "npm install ..."
    npm install
    if ($LASTEXITCODE -ne 0) { throw "npm install 失败" }
}

Write-Host "tsc --noEmit ..."
npx tsc --noEmit
if ($LASTEXITCODE -ne 0) { throw "类型检查失败" }

Write-Host "esbuild 打包 ..."
node build.mjs
if ($LASTEXITCODE -ne 0) { throw "esbuild 打包失败" }

$dist = Join-Path $Modules "dist\index.js"
if (-not (Test-Path $dist)) { throw "未生成 dist\index.js" }
Write-Host ("构建成功: {0} ({1} bytes)" -f $dist, (Get-Item $dist).Length) -ForegroundColor Green
