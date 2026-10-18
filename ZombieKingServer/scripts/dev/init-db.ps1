# ZombieKing P1：初始化本地 PostgreSQL（角色/库/迁移/种子）并生成加固后的 local.yml
# 用法: .\init-db.ps1 [-AdminUser postgres] [-AdminPassword xxx]
# 未提供 -AdminPassword 时会交互式提示输入（不回显）。

param(
    [string]$AdminUser = "postgres",
    [string]$AdminPassword = ""
)

$ErrorActionPreference = "Stop"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot   = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$EnvFile    = Join-Path $ScriptDir "dev.env"
$LocalYml   = Join-Path $RepoRoot "nakama\Data\local.yml"
$Migrations = Join-Path $RepoRoot "sql\migrations"
$SeedSql    = Join-Path $RepoRoot "sql\seed.sql"
$DistDir    = (Join-Path $RepoRoot "nakama\modules\dist") -replace '\\','/'

# ---------- 定位 psql ----------
function Find-Psql {
    $found = @()
    $found += Get-ChildItem "F:\APP\PostgreSQL\*\bin\psql.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
    $found += Get-ChildItem "C:\Program Files\PostgreSQL\*\bin\psql.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
    $cmd = Get-Command psql -ErrorAction SilentlyContinue
    if ($cmd) { $found += $cmd.Source }
    if ($found.Count -eq 0) { throw "未找到 psql.exe，请安装 PostgreSQL 或修正脚本中的查找路径" }
    return $found[0]
}
$Psql = Find-Psql
Write-Host "psql: $Psql"

# ---------- 生成/读取 dev.env（本地密钥唯一来源） ----------
function New-RandomHex([int]$ByteCount) {
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $buf = New-Object byte[] $ByteCount
        $rng.GetBytes($buf)
        return (($buf | ForEach-Object { $_.ToString("x2") }) -join "")
    } finally { $rng.Dispose() }
}
function New-RandomBase64([int]$ByteCount) {
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $buf = New-Object byte[] $ByteCount
        $rng.GetBytes($buf)
        return [Convert]::ToBase64String($buf)
    } finally { $rng.Dispose() }
}

if (-not (Test-Path $EnvFile)) {
    Write-Host "生成新密钥文件: $EnvFile"
    $lines = @(
        "ZK_DB_USER=nakama"
        "ZK_DB_NAME=zombieking"
        "ZK_DB_PORT=5432"
        ("ZK_DB_PASSWORD=" + (New-RandomHex 24))
        ("ZK_SOCKET_SERVER_KEY=" + (New-RandomHex 32))
        ("ZK_SESSION_ENCRYPTION_KEY=" + (New-RandomBase64 32))
        ("ZK_REFRESH_ENCRYPTION_KEY=" + (New-RandomBase64 32))
        "ZK_CONSOLE_USERNAME=zk_console"
        ("ZK_CONSOLE_PASSWORD=" + (New-RandomHex 16))
        ("ZK_CONSOLE_SIGNING_KEY=" + (New-RandomBase64 32))
        ("ZK_SERVICE_TOKEN=" + (New-RandomHex 32))
        ("ZK_RUNTIME_HTTP_KEY=" + (New-RandomHex 32))
    )
    Set-Content -Path $EnvFile -Value $lines -Encoding ASCII
}

$Env = @{}
Get-Content $EnvFile | ForEach-Object {
    if ($_ -match '^\s*([A-Z0-9_]+)=(.*)$') { $Env[$matches[1]] = $matches[2] }
}
foreach ($k in @("ZK_DB_USER","ZK_DB_NAME","ZK_DB_PASSWORD","ZK_DB_PORT","ZK_SOCKET_SERVER_KEY",
                 "ZK_SESSION_ENCRYPTION_KEY","ZK_REFRESH_ENCRYPTION_KEY","ZK_CONSOLE_USERNAME",
                 "ZK_CONSOLE_PASSWORD","ZK_CONSOLE_SIGNING_KEY")) {
    if (-not $Env.ContainsKey($k)) { throw "dev.env 缺少 $k" }
}

$DbUser = $Env["ZK_DB_USER"]
$DbName = $Env["ZK_DB_NAME"]
$DbPass = $Env["ZK_DB_PASSWORD"]
$DbPort = $Env["ZK_DB_PORT"]

# ---------- 管理员密码 ----------
if ($AdminPassword -eq "") {
    $secure = Read-Host "输入 PostgreSQL 超级用户($AdminUser)密码" -AsSecureString
    $AdminPassword = [Runtime.InteropServices.Marshal]::PtrToStringAuto(
        [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure))
}

function Invoke-Admin([string]$Sql) {
    $env:PGPASSWORD = $AdminPassword
    $env:PGCLIENTENCODING = "UTF8"
    & $Psql -U $AdminUser -h 127.0.0.1 -p $DbPort -w -d postgres -v ON_ERROR_STOP=1 -t -A -c $Sql
    if ($LASTEXITCODE -ne 0) { throw "管理员 SQL 执行失败: $Sql" }
}

function Invoke-App([string[]]$PsqlArgs) {
    $env:PGPASSWORD = $DbPass
    $env:PGCLIENTENCODING = "UTF8"
    & $Psql -U $DbUser -h 127.0.0.1 -p $DbPort -w -d $DbName -v ON_ERROR_STOP=1 @PsqlArgs
    if ($LASTEXITCODE -ne 0) { throw "nakama 用户 psql 执行失败" }
}

# ---------- 角色与库 ----------
$exists = Invoke-Admin "SELECT 1 FROM pg_roles WHERE rolname='$DbUser'"
if ($exists) {
    Write-Host "角色 $DbUser 已存在，更新密码"
    Invoke-Admin "ALTER ROLE $DbUser LOGIN PASSWORD '$DbPass'" | Out-Null
} else {
    Write-Host "创建角色 $DbUser"
    Invoke-Admin "CREATE ROLE $DbUser LOGIN PASSWORD '$DbPass'" | Out-Null
}
$exists = Invoke-Admin "SELECT 1 FROM pg_database WHERE datname='$DbName'"
if (-not $exists) {
    Write-Host "创建数据库 $DbName (owner=$DbUser)"
    Invoke-Admin "CREATE DATABASE $DbName OWNER $DbUser" | Out-Null
} else {
    Write-Host "数据库 $DbName 已存在"
}

# ---------- 迁移（按版本号增量应用，兼容 golang-migrate 命名） ----------
Invoke-App @("-q", "-c", "CREATE SCHEMA IF NOT EXISTS game")
Invoke-App @("-q", "-c", "CREATE TABLE IF NOT EXISTS game.schema_migrations (version integer PRIMARY KEY, filename text NOT NULL, applied_at timestamptz NOT NULL DEFAULT now())")

$applied = Invoke-App @("-t", "-A", "-c", "SELECT version FROM game.schema_migrations")
$appliedSet = @{}
foreach ($a in $applied) { if ($a) { $appliedSet[[int]$a] = $true } }

Get-ChildItem (Join-Path $Migrations "*.up.sql") | Sort-Object Name | ForEach-Object {
    $ver = [int]($_.Name -replace '^0*(\d+)_.*', '$1')
    if ($appliedSet.ContainsKey($ver)) {
        Write-Host ("跳过已应用: {0}" -f $_.Name)
    } else {
        Write-Host ("应用迁移: {0}" -f $_.Name)
        Invoke-App @("-q", "-f", $_.FullName)
        Invoke-App @("-t", "-A", "-c", ("INSERT INTO game.schema_migrations(version,filename) VALUES ({0},'{1}')" -f $ver, $_.Name)) | Out-Null
    }
}

# ---------- 种子数据（幂等） ----------
Write-Host "应用种子: sql\seed.sql"
Invoke-App @("-q", "-f", $SeedSql)

# 本地开发开启 dev_mode（生产部署不执行此步，sys_config 保持 false）
Invoke-App @("-q", "-c", "UPDATE game.sys_config SET value='true' WHERE key='dev_mode'")

# 撮合流配置：DS Manager 地址与服务 Token（生产由部署脚本注入真实值；注意保持 ASCII，-c 走控制台代码页）
Invoke-App @("-q", "-c", "INSERT INTO game.sys_config(key,value,note) VALUES ('ds_manager_url','http://127.0.0.1:8443','ds manager base url') ON CONFLICT (key) DO UPDATE SET value=EXCLUDED.value")
Invoke-App @("-q", "-c", ("INSERT INTO game.sys_config(key,value,note) VALUES ('ds_service_token','{0}','allocations service token') ON CONFLICT (key) DO UPDATE SET value=EXCLUDED.value" -f $Env["ZK_SERVICE_TOKEN"]))

# 大厅频道：lobby-bot 经本机 REST 建组所需的 server key 与自身地址（F2）
Invoke-App @("-q", "-c", ("INSERT INTO game.sys_config(key,value,note) VALUES ('socket_server_key','{0}','lobby-bot self REST auth') ON CONFLICT (key) DO UPDATE SET value=EXCLUDED.value" -f $Env["ZK_SOCKET_SERVER_KEY"]))
Invoke-App @("-q", "-c", "INSERT INTO game.sys_config(key,value,note) VALUES ('lobby_self_url','http://127.0.0.1:7350','nakama own REST base') ON CONFLICT (key) DO UPDATE SET value=EXCLUDED.value")

# ---------- 生成加固后的 local.yml（替代默认占位配置） ----------
Write-Host "生成 $LocalYml"
$yml = @"
# ZombieKing local dev config - generated by scripts/dev/init-db.ps1, secrets in scripts/dev/dev.env
logger:
  level: "info"
  stdout: true

database:
  address:
    - "postgres://${DbUser}:${DbPass}@127.0.0.1:${DbPort}/${DbName}?sslmode=disable"

session:
  encryption_key: "$($Env["ZK_SESSION_ENCRYPTION_KEY"])"
  refresh_encryption_key: "$($Env["ZK_REFRESH_ENCRYPTION_KEY"])"
  token_expiry_sec: 900
  refresh_token_expiry_sec: 259200

socket:
  server_key: "$($Env["ZK_SOCKET_SERVER_KEY"])"
  port: 7350
  max_message_size_bytes: 4096

console:
  address: "127.0.0.1"
  port: 7351
  username: "$($Env["ZK_CONSOLE_USERNAME"])"
  password: "$($Env["ZK_CONSOLE_PASSWORD"])"
  signing_key: "$($Env["ZK_CONSOLE_SIGNING_KEY"])"

runtime:
  path: "${DistDir}"
  js_entrypoint: "index.js"
  http_key: "$($Env["ZK_RUNTIME_HTTP_KEY"])"
  env:
    - "DEV_MODE=true"
"@
Set-Content -Path $LocalYml -Value $yml -Encoding ASCII

# ---------- Nakama 内部迁移（users/device_storage 等系统表） ----------
$NakamaExe = Join-Path $RepoRoot "nakama\nakama.exe"
if (Test-Path $NakamaExe) {
    Write-Host "执行 nakama migrate up ..."
    Push-Location (Join-Path $RepoRoot "nakama")
    & $NakamaExe migrate up --config $LocalYml 2>&1 | Select-Object -Last 2
    $migOk = ($LASTEXITCODE -eq 0)
    Pop-Location
    if (-not $migOk) { throw "nakama migrate up 失败" }
} else {
    Write-Warning "未找到 nakama.exe，请手动执行: nakama.exe migrate up --config $LocalYml"
}

Remove-Item Env:PGPASSWORD -ErrorAction SilentlyContinue
Write-Host ""
Write-Host "完成。下一步: 运行 build-modules.ps1 与 start-nakama.ps1" -ForegroundColor Green
