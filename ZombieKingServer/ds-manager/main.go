// ZombieKing DS Manager（架构文档 §4.5）
// MVP：主机注册表驻内存（生产接 game.ds_host/ds_instance 持久化，接口已对齐）。
// 纯 stdlib，无第三方依赖。
package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

type Host struct {
	HostID   string `json:"host_id"`
	IP       string `json:"ip"`
	AgentURL string `json:"agent_url"`
	MaxDS    int    `json:"max_ds"`
	Active   int
	Secret   string    // dev 明文；生产为解密后的 host_secret
	LastSeen time.Time // register/heartbeat 兼心跳，超时清理
	// R4 可疑主机隔离（由 refreshHostFlags 从 game.ds_host 同步）
	Suspicious int  `json:"-"`
	Isolated   bool `json:"-"`
}

type Instance struct {
	HostID string
	Port   int
}

type AllocationReq struct {
	MatchID  string           `json:"match_id"`
	Mode     string           `json:"mode"`
	Map      string           `json:"map"`
	Duration int              `json:"duration_sec"`
	Players  []json.RawMessage `json:"players"`
}

type AllocationResp struct {
	DsIP     string `json:"ds_ip"`
	DsPort   int    `json:"ds_port"`
	DsID     string `json:"ds_id"`
	DsSecret string `json:"ds_secret"`
	HostID   string `json:"ds_host_id"`
}

var (
	mu           sync.Mutex
	hosts        = map[string]*Host{}
	instances    = map[string]Instance{} // ds_id -> 实例(主机/端口)，退出回收
	seq          int64
	portMin      = 7777
	portMax      = 7877
	usedPorts    = map[int]bool{}
	serviceToken = os.Getenv("ZK_SERVICE_TOKEN")
	// host_id=secret 逗号分隔
	hostSecrets = parseSecrets(os.Getenv("ZK_HOST_SECRETS"))
	hostTimeout = 30 * time.Second
)

func parseSecrets(s string) map[string]string {
	m := map[string]string{}
	for _, kv := range strings.Split(s, ",") {
		p := strings.SplitN(kv, "=", 2)
		if len(p) == 2 {
			m[strings.TrimSpace(p[0])] = strings.TrimSpace(p[1])
		}
	}
	return m
}

func uuidv4() string {
	b := make([]byte, 16)
	rand.Read(b)
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}

func hmacHex(key, data string) string {
	m := hmac.New(sha256.New, []byte(key))
	m.Write([]byte(data))
	return hex.EncodeToString(m.Sum(nil))
}

func readBody(w http.ResponseWriter, r *http.Request, max int64) ([]byte, bool) {
	b, err := io.ReadAll(io.LimitReader(r.Body, max))
	if err != nil {
		http.Error(w, "read error", http.StatusBadRequest)
		return nil, false
	}
	return b, true
}

// POST /v1/hosts/register — ds-agent 以主机预置密钥 HMAC 注册
func handleRegister(w http.ResponseWriter, r *http.Request) {
	body, ok := readBody(w, r, 1<<16)
	if !ok { return }
	var h Host
	if err := json.Unmarshal(body, &h); err != nil || h.HostID == "" {
		http.Error(w, "bad json", http.StatusBadRequest); return
	}
	secret, known := hostSecrets[h.HostID]
	if !known { http.Error(w, "unknown host", http.StatusForbidden); return }
	want := hmacHex(secret, string(body)+r.Header.Get("X-Seq"))
	if !hmac.Equal([]byte(want), []byte(r.Header.Get("X-Sig"))) {
		log.Printf("register HMAC 校验失败 host=%s", h.HostID)
		http.Error(w, "bad sig", http.StatusUnauthorized); return
	}
	mu.Lock()
	h.Secret = secret
	if h.MaxDS == 0 { h.MaxDS = 8 }
	if h.AgentURL == "" { h.AgentURL = "http://" + h.IP + ":8444" }
	if old, ok := hosts[h.HostID]; ok {
		h.Active = old.Active // 心跳重注册保留在跑实例计数
	}
	h.LastSeen = time.Now()
	hosts[h.HostID] = &h
	mu.Unlock()
	persistHostRegister(&h)
	w.Write([]byte(`{"ok":true}`))
}

// POST /v1/hosts/ds-exit — agent 上报 DS 进程退出，回收 active 与端口（host secret HMAC）
func handleDsExit(w http.ResponseWriter, r *http.Request) {
	body, ok := readBody(w, r, 1<<16)
	if !ok { return }
	var req struct {
		HostID string `json:"host_id"`
		DsID   string `json:"ds_id"`
	}
	if err := json.Unmarshal(body, &req); err != nil {
		http.Error(w, "bad json", http.StatusBadRequest); return
	}
	secret, known := hostSecrets[req.HostID]
	if !known { http.Error(w, "unknown host", http.StatusForbidden); return }
	want := hmacHex(secret, string(body)+r.Header.Get("X-Seq"))
	if !hmac.Equal([]byte(want), []byte(r.Header.Get("X-Sig"))) {
		http.Error(w, "bad sig", http.StatusUnauthorized); return
	}
	mu.Lock()
	if inst, exists := instances[req.DsID]; exists {
		delete(instances, req.DsID)
		delete(usedPorts, inst.Port)
		if h, ok := hosts[inst.HostID]; ok && h.Active > 0 {
			h.Active--
		}
		log.Printf("ds %s exited, host=%s port=%d reclaimed", req.DsID, inst.HostID, inst.Port)
	}
	mu.Unlock()
	persistInstanceExit(req.DsID)
	w.Write([]byte(`{"ok":true}`))
}

// 失联清理：超过 hostTimeout 未心跳的主机移出注册表并回收其端口计数；周期同步隔离状态
func reaper() {
	for {
		time.Sleep(5 * time.Second)
		refreshHostFlags()
		mu.Lock()
		for id, h := range hosts {
			if time.Since(h.LastSeen) > hostTimeout {
				log.Printf("host %s 失联，摘除", id)
				for ds, inst := range instances {
					if inst.HostID == id {
						delete(instances, ds)
						delete(usedPorts, inst.Port)
					}
				}
				delete(hosts, id)
				persistHostOffline(id)
			}
		}
		mu.Unlock()
	}
}

// POST /v1/allocations — Nakama(或测试) 以服务 Token 调用
func handleAlloc(w http.ResponseWriter, r *http.Request) {
	if r.Header.Get("X-Service-Token") != serviceToken {
		http.Error(w, "unauthorized", http.StatusUnauthorized); return
	}
	body, ok := readBody(w, r, 1<<20)
	if !ok { return }
	var req AllocationReq
	if err := json.Unmarshal(body, &req); err != nil || req.MatchID == "" || len(req.Players) == 0 {
		http.Error(w, "bad json", http.StatusBadRequest); return
	}

	dsID := uuidv4()

	mu.Lock()
	var best *Host
	for _, h := range hosts {
		if h.Isolated || h.Suspicious >= suspLimit {
			continue // R4：可疑达阈/已隔离主机不参与分配
		}
		if h.Active < h.MaxDS && (best == nil || h.Active < best.Active) {
			best = h
		}
	}
	if best == nil {
		mu.Unlock()
		http.Error(w, "no available ds host", http.StatusServiceUnavailable); return
	}
	port := 0
	for p := portMin; p <= portMax; p++ {
		if !usedPorts[p] { port = p; break }
	}
	if port == 0 { mu.Unlock(); http.Error(w, "port exhausted", http.StatusServiceUnavailable); return }
	usedPorts[port] = true
	best.Active++
	instances[dsID] = Instance{HostID: best.HostID, Port: port}
	seq++
	mu.Unlock()
	mySeq := nextCmdSeq(seq) // 跨重启单调（agent 防重放）

	dsSecret := hmacHex(best.Secret, req.MatchID)

	// 向 Agent 下发 SpawnDS（HMAC + 单调序号防重放）
	cmd := map[string]interface{}{
		"ds_id": dsID, "port": port, "match_id": req.MatchID,
		"ds_secret": dsSecret, "players": req.Players,
		"duration_sec": req.Duration,
	}
	cmdBody, _ := json.Marshal(cmd)
	cmdSeq := fmt.Sprintf("%d", mySeq)
	httpReq, _ := http.NewRequest("POST", best.AgentURL+"/commands/spawn", strings.NewReader(string(cmdBody)))
	httpReq.Header.Set("Content-Type", "application/json")
	httpReq.Header.Set("X-Seq", cmdSeq)
	httpReq.Header.Set("X-Sig", hmacHex(best.Secret, string(cmdBody)+cmdSeq))
	cli := &http.Client{Timeout: 5 * time.Second}
	resp, err := cli.Do(httpReq)
	if err != nil {
		mu.Lock(); best.Active--; delete(usedPorts, port); delete(instances, dsID); mu.Unlock()
		log.Printf("spawn 失败 host=%s err=%v", best.HostID, err)
		http.Error(w, "agent unreachable", http.StatusBadGateway); return
	}
	resp.Body.Close()
	if resp.StatusCode != 200 {
		mu.Lock(); best.Active--; delete(usedPorts, port); delete(instances, dsID); mu.Unlock()
		http.Error(w, "agent rejected command", http.StatusBadGateway); return
	}

	log.Printf("allocated match=%s host=%s port=%d ds=%s", req.MatchID, best.HostID, port, dsID)
	persistInstance(dsID, best.HostID, req.MatchID, port)
	out, _ := json.Marshal(AllocationResp{DsIP: best.IP, DsPort: port, DsID: dsID, DsSecret: dsSecret, HostID: best.HostID})
	w.Header().Set("Content-Type", "application/json")
	w.Write(out)
}

func handleState(w http.ResponseWriter, r *http.Request) {
	mu.Lock()
	type hs struct { HostID string `json:"host_id"`; IP string `json:"ip"`; Active int `json:"active"`; Max int `json:"max_ds"` }
	var list []hs
	for _, h := range hosts {
		list = append(list, hs{h.HostID, h.IP, h.Active, h.MaxDS})
	}
	mu.Unlock()
	out, _ := json.Marshal(list)
	w.Write(out)
}

func main() {
	if serviceToken == "" || len(hostSecrets) == 0 {
		log.Fatal("需要环境变量 ZK_SERVICE_TOKEN 与 ZK_HOST_SECRETS(host=secret,...)")
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/hosts/register", handleRegister)
	mux.HandleFunc("/v1/hosts/ds-exit", handleDsExit)
	mux.HandleFunc("/v1/hosts/recover", handleRecover)
	mux.HandleFunc("/v1/allocations", handleAlloc)
	mux.HandleFunc("/v1/state", handleState)
	initDB()
	loadState()
	go reaper()
	addr := os.Getenv("ZK_LISTEN")
	if addr == "" { addr = ":8443" }
	log.Printf("ds-manager listening %s", addr)
	log.Fatal(http.ListenAndServe(addr, mux))
}
