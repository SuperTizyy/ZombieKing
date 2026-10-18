#!/usr/bin/env bash
# =============================================================================
# ZombieKing 数据库备份（架构文档 §8 承诺：每日备份 + 保留策略）
#
# 用法（Linux 宿主机，deploy/ 目录或任意位置）:
#   ./backup-db.sh                 # 使用同目录 .env 的 ZK_DB_* 与 ZK_BACKUP_* 配置
#
# cron 示例（每日 03:30，日志进 /var/log/zk-backup.log）:
#   30 3 * * * cd /opt/zombieking/deploy && ./backup-db.sh >> /var/log/zk-backup.log 2>&1
#
# 纪律：备份文件权限 600；先写 .tmp 再原子改名，避免截断文件混入保留集；
#       恢复演练口径：psql -U $ZK_DB_USER -d $ZK_DB_NAME < 解压后的 sql。
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPLOY_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")/deploy"

# 读取 .env（仅取 ZK_ 前缀，避免污染）
if [[ -f "$DEPLOY_DIR/.env" ]]; then
    set -a; source "$DEPLOY_DIR/.env"; set +a
fi

: "${ZK_DB_NAME:=zombieking}"
: "${ZK_DB_USER:=nakama}"
: "${ZK_BACKUP_DIR:=/var/backups/zombieking}"
: "${ZK_BACKUP_KEEP_DAYS:=14}"

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$ZK_BACKUP_DIR/$ZK_DB_NAME-$STAMP.sql.gz"
mkdir -p "$ZK_BACKUP_DIR"
chmod 700 "$ZK_BACKUP_DIR"

# 经 compose 内 postgres 容器执行 pg_dump（无需宿主机装客户端；密码在容器环境内）
docker compose -f "$DEPLOY_DIR/docker-compose.yml" exec -T postgres \
    pg_dump -U "$ZK_DB_USER" -d "$ZK_DB_NAME" --no-owner --format=plain \
    | gzip > "$OUT.tmp"
mv "$OUT.tmp" "$OUT"
chmod 600 "$OUT"
echo "backup ok: $OUT ($(du -h "$OUT" | cut -f1))"

# 保留策略：删除 N 天前的备份
find "$ZK_BACKUP_DIR" -name "*.sql.gz" -mtime "+$ZK_BACKUP_KEEP_DAYS" -delete
echo "pruned, keep=${ZK_BACKUP_KEEP_DAYS}d"
