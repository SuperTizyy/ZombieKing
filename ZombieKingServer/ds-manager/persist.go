// DS Manager 持久化（架构文档 §4.5 承诺）：主机注册表与 DS 实例落 game.ds_host/ds_instance。
// ZK_DB_DSN 为空时退回纯内存模式（向后兼容）。
// 密钥管理：host_secret 仍由环境变量注入（DB 只存 sha256 哈希做完整性记录，绝不明文入库）。
package main

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"log"
	"net/http"
	"os"
	"strconv"
	"time"

	_ "github.com/lib/pq"
)

var db *sql.DB

var suspLimit = func() int {
	if v, err := strconv.Atoi(os.Getenv("ZK_SUSPICIOUS_LIMIT")); err == nil && v > 0 {
		return v
	}
	return 3 // 可疑计数阈值：达到即隔离
}()

func initDB() {
	dsn := os.Getenv("ZK_DB_DSN")
	if dsn == "" {
		log.Printf("ZK_DB_DSN 未设置：纯内存模式（重启丢状态）")
		return
	}
	var err error
	db, err = sql.Open("postgres", dsn)
	if err != nil {
		log.Fatalf("db open: %v", err)
	}
	if err = db.Ping(); err != nil {
		log.Fatalf("db ping: %v", err)
	}
	// 指令序号基线：必须高于历史 agent 已见 seq（旧内存版 manager 未持久化，取大基线）；
	// 用独立表避免 game.sys_config.value 的 CHECK 长度约束
	if _, err := db.Exec(`CREATE TABLE IF NOT EXISTS game.ds_cmd_seq (id int PRIMARY KEY, value bigint NOT NULL)`); err != nil {
		log.Fatalf("init ds_cmd_seq tbl: %v", err)
	}
	if _, err := db.Exec(`INSERT INTO game.ds_cmd_seq(id,value) VALUES (1,1000000) ON CONFLICT (id) DO NOTHING`); err != nil {
		log.Fatalf("init ds_cmd_seq: %v", err)
	}
	log.Printf("ds-manager 持久化已启用 (game.ds_host/ds_instance)")
}

func secretHash(secret string) string {
	h := sha256.Sum256([]byte(secret))
	return "sha256:" + hex.EncodeToString(h[:])
}

// nextCmdSeq 跨重启单调指令序号（agent 防重放要求严格递增；重启后内存 seq 归零会被 agent 拒绝）
func nextCmdSeq(memSeq int64) int64 {
	if db == nil {
		return memSeq
	}
	var v int64
	err := db.QueryRow(`
		INSERT INTO game.ds_cmd_seq(id,value) VALUES (1, $1+1)
		ON CONFLICT (id) DO UPDATE SET value = GREATEST(game.ds_cmd_seq.value, $1) + 1
		RETURNING value`, memSeq).Scan(&v)
	if err != nil {
		log.Printf("nextCmdSeq: %v", err)
		return memSeq
	}
	return v
}

// loadState 启动恢复：active 主机（secret 仍来自 env）与 running 实例（active 计数/端口占用）
func loadState() {
	if db == nil {
		return
	}
	rows, err := db.Query(`SELECT host_id, ip, max_ds FROM game.ds_host WHERE status='active'`)
	if err != nil {
		log.Printf("loadState hosts: %v", err)
		return
	}
	defer rows.Close()
	for rows.Next() {
		var id, ip string
		var maxDS int
		if err := rows.Scan(&id, &ip, &maxDS); err != nil {
			continue
		}
		secret, known := hostSecrets[id]
		if !known {
			log.Printf("loadState: 主机 %s 在库中但 env 无密钥，跳过", id)
			continue
		}
		hosts[id] = &Host{HostID: id, IP: ip, MaxDS: maxDS, Secret: secret, AgentURL: "http://" + ip + ":8444", LastSeen: time.Time{}} // 零值=待 agent 重注册确认
	}
	irows, err := db.Query(`SELECT ds_id::text, host_id, port FROM game.ds_instance WHERE status='running'`)
	if err != nil {
		log.Printf("loadState instances: %v", err)
		return
	}
	defer irows.Close()
	for irows.Next() {
		var dsID, hostID string
		var port int
		if err := irows.Scan(&dsID, &hostID, &port); err != nil {
			continue
		}
		instances[dsID] = Instance{HostID: hostID, Port: port}
		usedPorts[port] = true
		if h, ok := hosts[hostID]; ok {
			h.Active++
		}
	}
	log.Printf("loadState 恢复: hosts=%d instances=%d", len(hosts), len(instances))
}

func persistHostRegister(h *Host) {
	if db == nil {
		return
	}
	_, err := db.Exec(`
		INSERT INTO game.ds_host(host_id, ip, agent_secret_enc, status, max_ds, last_heartbeat)
		VALUES ($1,$2,$3,'active',$4, now())
		ON CONFLICT (host_id) DO UPDATE SET ip=EXCLUDED.ip, max_ds=EXCLUDED.max_ds, last_heartbeat=now()`,
		h.HostID, h.IP, secretHash(h.Secret), h.MaxDS)
	if err != nil {
		log.Printf("persistHostRegister: %v", err)
	}
}

func persistInstance(dsID, hostID, matchID string, port int) {
	if db == nil {
		return
	}
	_, err := db.Exec(`
		INSERT INTO game.ds_instance(ds_id, host_id, match_id, port, status, updated_at)
		VALUES ($1::uuid,$2,$3::uuid,$4,'running',now())`,
		dsID, hostID, matchID, port)
	if err != nil {
		log.Printf("persistInstance: %v", err)
	}
}

func persistInstanceExit(dsID string) {
	if db == nil {
		return
	}
	if _, err := db.Exec(`UPDATE game.ds_instance SET status='exited', updated_at=now() WHERE ds_id=$1::uuid`, dsID); err != nil {
		log.Printf("persistInstanceExit: %v", err)
	}
}

func persistHostOffline(hostID string) {
	if db == nil {
		return
	}
	if _, err := db.Exec(`UPDATE game.ds_host SET status='offline' WHERE host_id=$1 AND status='active'`, hostID); err != nil {
		log.Printf("persistHostOffline: %v", err)
	}
}

// refreshHostFlags 周期同步可疑计数/状态：suspicious_count 达阈值自动隔离（Nakama 结算复验失败时累加）
func refreshHostFlags() {
	if db == nil {
		return
	}
	if _, err := db.Exec(`UPDATE game.ds_host SET status='isolated' WHERE status='active' AND suspicious_count >= $1`, suspLimit); err != nil {
		log.Printf("auto-isolate: %v", err)
	}
	rows, err := db.Query(`SELECT host_id, status, suspicious_count FROM game.ds_host`)
	if err != nil {
		log.Printf("refreshHostFlags: %v", err)
		return
	}
	defer rows.Close()
	for rows.Next() {
		var id, status string
		var susp int
		if err := rows.Scan(&id, &status, &susp); err != nil {
			continue
		}
		mu.Lock()
		if h, ok := hosts[id]; ok {
			h.Suspicious = susp
			h.Isolated = (status == "isolated")
		}
		mu.Unlock()
	}
}

// POST /v1/hosts/recover — 运维经服务 Token 解除隔离并清零计数
func handleRecover(w http.ResponseWriter, r *http.Request) {
	if r.Header.Get("X-Service-Token") != serviceToken {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	if db == nil {
		http.Error(w, "persistence disabled (memory mode)", http.StatusNotImplemented)
		return
	}
	body, ok := readBody(w, r, 1<<16)
	if !ok {
		return
	}
	var req struct {
		HostID string `json:"host_id"`
	}
	if err := json.Unmarshal(body, &req); err != nil || req.HostID == "" {
		http.Error(w, "bad json", http.StatusBadRequest)
		return
	}
	res, err := db.Exec(`UPDATE game.ds_host SET status='active', suspicious_count=0 WHERE host_id=$1`, req.HostID)
	if err != nil {
		http.Error(w, "db error", http.StatusInternalServerError)
		return
	}
	if n, _ := res.RowsAffected(); n == 0 {
		http.Error(w, "host not found", http.StatusNotFound)
		return
	}
	mu.Lock()
	if h, ok := hosts[req.HostID]; ok {
		h.Isolated = false
		h.Suspicious = 0
	}
	mu.Unlock()
	log.Printf("host %s recovered", req.HostID)
	w.Write([]byte(`{"ok":true}`))
}
