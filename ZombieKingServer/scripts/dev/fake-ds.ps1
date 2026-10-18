# 假 DS（Go 环境就绪前的结算链路验证工具；模拟受信任 DS 行为）
# 用法:
#   结算上报:  .\fake-ds.ps1 -MatchId <uuid> -DsId <uuid> -DsSecret <hex> -HttpKey <key> -PlayersJson '[{"user_id":"...","team":1,"stats":{...}},...]'
#   篡改签名:  追加 -Tamper
#   验证 join_token(模拟 PreLogin): .\fake-ds.ps1 -DsSecret <hex> -VerifyToken '<payload>.<sig>'
param(
    [string]$MatchId = "",
    [string]$DsId = "",
    [string]$DsSecret = "",
    [string]$HttpKey = "",
    [string]$Nakama = "http://127.0.0.1:7350",
    [string]$PlayersJson = "",
    [int]$WinnerTeam = 1,
    [switch]$Tamper,
    [string]$VerifyToken = "",
    [int]$DelaySec = 0,
    [string]$PlayersFile = "",
    [string]$OutcomeJson = ""
)
$ErrorActionPreference = "Stop"

function Get-HexFromBytes($bytes) { ($bytes | ForEach-Object { $_.ToString("x2") }) -join "" }
function Get-HmacHex($text, $key) {
    $h = New-Object System.Security.Cryptography.HMACSHA256
    $h.Key = [Text.Encoding]::UTF8.GetBytes($key)
    return Get-HexFromBytes ($h.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))
}

# 含引号的 JSON 参数经命令行传递会被破坏，支持从环境变量/文件读取
if ($VerifyToken -eq "ENV") { $VerifyToken = $env:ZK_VERIFY_TOKEN }
if ($PlayersJson -eq "ENV") { $PlayersJson = $env:ZK_PLAYERS_JSON }
if ($PlayersFile -ne "") { $PlayersJson = (Get-Content $PlayersFile -Raw) }
if ($DelaySec -gt 0) { Write-Output "delaying ${DelaySec}s before settle"; Start-Sleep -Seconds $DelaySec }

# ---- 模式 1：模拟 DS PreLogin 验签 join_token ----
if ($VerifyToken -ne "") {
    $idx = $VerifyToken.LastIndexOf(".")
    $payload = $VerifyToken.Substring(0, $idx)
    $sig = $VerifyToken.Substring($idx + 1)
    $expect = Get-HmacHex $payload $DsSecret
    if ($expect -eq $sig) {
        Write-Output "TOKEN_SIG_OK"
        Write-Output $payload
    } else {
        Write-Output "TOKEN_SIG_BAD"
        exit 1
    }
    exit 0
}

# ---- 模式 2：结算上报 ----
if ($MatchId -eq "" -or $DsId -eq "" -or $DsSecret -eq "" -or $PlayersJson -eq "") {
    throw "需要 -MatchId -DsId -DsSecret -PlayersJson（或用 -VerifyToken 验签模式）"
}
$settledAt = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()

# payload_hash = sha256(players JSON 原文)
$sha = [System.Security.Cryptography.SHA256]::Create()
$payloadHash = Get-HexFromBytes ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($PlayersJson)))

# canonical = match_id|ds_id|settled_at|payload_hash
$canon = "$MatchId|$DsId|$settledAt|$payloadHash"
$sig = Get-HmacHex $canon $DsSecret
if ($Tamper) {
    $last = $sig.Substring($sig.Length - 1)
    $flip = if ($last -eq "f") { "e" } else { "f" }
    $sig = $sig.Substring(0, $sig.Length - 1) + $flip
}

$outcome = if ($OutcomeJson -ne "") { $OutcomeJson } else { '{"winner_team":' + $WinnerTeam + '}' }
$body = '{"match_id":"' + $MatchId + '","ds_id":"' + $DsId + '","settled_at":' + $settledAt +
        ',"outcome":' + $outcome + ',"players":' + $PlayersJson +
        ',"payload_hash":"' + $payloadHash + '","sig":"' + $sig + '"}'

# Nakama REST RPC 的 body 必须是 JSON 字符串（双重编码）
$rpcBody = $body | ConvertTo-Json
$bf = [IO.Path]::GetTempFileName()
[IO.File]::WriteAllText($bf, $rpcBody)

$url = "$Nakama/v2/rpc/rpc_match_settle"
if ($HttpKey -ne "") { $url += "?http_key=$HttpKey" }
Write-Output "POST $url"
curl.exe -s -X POST $url -H "Content-Type: application/json" --data-binary "@$bf"
Remove-Item $bf
