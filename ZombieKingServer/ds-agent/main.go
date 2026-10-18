// ZombieKing ds-agent：DS 主机常驻代理（架构文档 §4.5）
// 注册/心跳到 DS Manager；接收 SpawnDS 指令（HMAC+单调序号防重放）；按命令模板拉起 DS 进程。
package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

var (
	hostID     = os.Getenv("ZK_HOST_ID")
	hostSecret = os.Getenv("ZK_HOST_SECRET")
	managerURL = os.Getenv("ZK_MANAGER_URL")
	agentIP    = os.Getenv("ZK_HOST_IP")
	dsCommand  = os.Getenv("ZK_DS_COMMAND") // 模板占位: {match_id} {ds_id} {port} {ds_secret} {players_file}
	outSeq     int64 // 出站(注册/心跳)序号
	cmdSeqSeen int64 // 入站指令序号（防重放，独立计数空间）
	mu         sync.Mutex
	procs      = map[string]*exec.Cmd{}
)

func hmacHex(key, data string) string {
	m := hmac.New(sha256.New, []byte(key))
	m.Write([]byte(data))
	return hex.EncodeToString(m.Sum(nil))
}

func signedPost(url string, body []byte) (*http.Response, error) {
	mu.Lock()
	outSeq++
	seq := outSeq
	mu.Unlock()
	req, err := http.NewRequest("POST", url, bytes.NewReader(body))
	if err != nil { return nil, err }
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Seq", strconv.FormatInt(seq, 10))
	req.Header.Set("X-Sig", hmacHex(hostSecret, string(body)+strconv.FormatInt(seq, 10)))
	return http.DefaultClient.Do(req)
}

func registerLoop() {
	port := strings.TrimPrefix(os.Getenv("ZK_AGENT_LISTEN"), ":")
	if port == "" { port = "8444" }
	body, _ := json.Marshal(map[string]interface{}{
		"host_id": hostID, "ip": agentIP, "max_ds": 8,
		"agent_url": "http://" + agentIP + ":" + port,
	})
	registered := false
	for {
		resp, err := signedPost(managerURL+"/v1/hosts/register", body)
		if err == nil {
			resp.Body.Close()
			if resp.StatusCode == 200 {
				if !registered { log.Printf("registered to %s", managerURL) }
				registered = true
			}
		} else if !registered {
			log.Printf("register err: %v", err)
		}
		time.Sleep(10 * time.Second) // 兼作心跳（register 幂等更新）
	}
}

// POST /commands/spawn — 校验 Manager 指令签名与序号，拉起 DS
func handleSpawn(w http.ResponseWriter, r *http.Request) {
	body, _ := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	seq, _ := strconv.ParseInt(r.Header.Get("X-Seq"), 10, 64)
	want := hmacHex(hostSecret, string(body)+r.Header.Get("X-Seq"))
	if !hmac.Equal([]byte(want), []byte(r.Header.Get("X-Sig"))) {
		log.Printf("spawn 指令签名无效，拒绝")
		http.Error(w, "bad sig", http.StatusUnauthorized)
		return
	}
	mu.Lock()
	if seq <= cmdSeqSeen {
		mu.Unlock()
		log.Printf("spawn 指令序号过旧 seq=%d last=%d，拒绝(防重放)", seq, cmdSeqSeen)
		http.Error(w, "replay", http.StatusUnauthorized)
		return
	}
	cmdSeqSeen = seq
	mu.Unlock()

	var cmd struct {
		DSID      string            `json:"ds_id"`
		MatchID   string            `json:"match_id"`
		Port      int               `json:"port"`
		DsSecret  string            `json:"ds_secret"`
		Players   []json.RawMessage `json:"players"`
	}
	if err := json.Unmarshal(body, &cmd); err != nil {
		http.Error(w, "bad json", http.StatusBadRequest); return
	}

	pf := filepath.Join(os.TempDir(), "zk-players-"+cmd.DSID+".json")
	pj, _ := json.Marshal(cmd.Players)
	if err := os.WriteFile(pf, pj, 0600); err != nil {
		http.Error(w, "write players failed", http.StatusInternalServerError); return
	}

	line := strings.NewReplacer(
		"{match_id}", cmd.MatchID, "{ds_id}", cmd.DSID,
		"{port}", strconv.Itoa(cmd.Port), "{ds_secret}", cmd.DsSecret,
		"{players_file}", pf,
	).Replace(dsCommand)
	parts := strings.Fields(line)
	c := exec.Command(parts[0], parts[1:]...)
	c.Stdout = os.Stdout
	c.Stderr = os.Stderr
	if err := c.Start(); err != nil {
		log.Printf("spawn 失败: %v", err)
		http.Error(w, "spawn failed", http.StatusInternalServerError); return
	}
	mu.Lock()
	procs[cmd.DSID] = c
	mu.Unlock()
	go func() {
		err := c.Wait()
		mu.Lock(); delete(procs, cmd.DSID); mu.Unlock()
		log.Printf("ds %s 退出: %v", cmd.DSID, err)
		// 上报 Manager 回收 active 与端口
		exitBody, _ := json.Marshal(map[string]string{"host_id": hostID, "ds_id": cmd.DSID})
		if resp, e := signedPost(managerURL+"/v1/hosts/ds-exit", exitBody); e == nil {
			resp.Body.Close()
			log.Printf("ds-exit report status %d", resp.StatusCode)
		} else {
			log.Printf("ds-exit report err: %v", e)
		}
	}()
	log.Printf("spawned ds %s on port %d (match %s)", cmd.DSID, cmd.Port, cmd.MatchID)
	fmt.Fprint(w, `{"ok":true}`)
}

func main() {
	if hostID == "" || hostSecret == "" || managerURL == "" || dsCommand == "" {
		log.Fatal("需要环境变量 ZK_HOST_ID ZK_HOST_SECRET ZK_MANAGER_URL ZK_DS_COMMAND (可选 ZK_HOST_IP)")
	}
	if agentIP == "" { agentIP = "127.0.0.1" }
	listen := os.Getenv("ZK_AGENT_LISTEN")
	if listen == "" { listen = ":8444" }
	go registerLoop()
	mux := http.NewServeMux()
	mux.HandleFunc("/commands/spawn", handleSpawn)
	log.Printf("ds-agent %s listening %s", hostID, listen)
	log.Fatal(http.ListenAndServe(listen, mux))
}
