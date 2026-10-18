// queuebot: 撮合流端到端验证（Nakama WebSocket JSON 协议最小实现，纯 stdlib）
// 流程: 设备登录 → WS 连接 → matchmaker_add → 等 matchmaker_matched → match_join → 等 match_data(op=100) 收 join_token
package main

import (
	"bufio"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

// ---- 最小 WS 客户端 ----

type wsConn struct {
	conn net.Conn
	br   *bufio.Reader
	mu   sync.Mutex
}

func wsDial(url string, header map[string]string) (*wsConn, error) {
	// url: ws://host:port/path
	keyBytes := make([]byte, 16)
	rand.Read(keyBytes)
	key := base64.StdEncoding.EncodeToString(keyBytes)
	host := strings.TrimPrefix(strings.TrimPrefix(url, "ws://"), "/")
	path := "/"
	if i := strings.Index(host, "/"); i >= 0 {
		path = host[i:]
		host = host[:i]
	}
	conn, err := net.Dial("tcp", host)
	if err != nil { return nil, err }
	req := "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
		"Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n"
	for k, v := range header { req += k + ": " + v + "\r\n" }
	req += "\r\n"
	if _, err = conn.Write([]byte(req)); err != nil { return nil, err }
	br := bufio.NewReader(conn)
	status, err := br.ReadString('\n')
	if err != nil { return nil, err }
	if !strings.Contains(status, "101") { return nil, fmt.Errorf("handshake failed: %s", status) }
	for {
		line, err := br.ReadString('\n')
		if err != nil { return nil, err }
		if line == "\r\n" { break }
	}
	return &wsConn{conn: conn, br: br}, nil
}

func (w *wsConn) WriteText(s string) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	payload := []byte(s)
	h := []byte{0x81}
	n := len(payload)
	switch {
	case n <= 125: h = append(h, byte(0x80|n))
	case n <= 65535: h = append(h, 0xFE); var b [2]byte; binary.BigEndian.PutUint16(b[:], uint16(n)); h = append(h, b[:]...)
	default: h = append(h, 0xFF); var b [8]byte; binary.BigEndian.PutUint64(b[:], uint64(n)); h = append(h, b[:]...)
	}
	mask := make([]byte, 4)
	rand.Read(mask)
	h = append(h, mask...)
	masked := make([]byte, n)
	for i := 0; i < n; i++ { masked[i] = payload[i] ^ mask[i%4] }
	if _, err := w.conn.Write(append(h, masked...)); err != nil { return err }
	return nil
}

func (w *wsConn) ReadText(timeout time.Duration) (string, error) {
	w.conn.SetReadDeadline(time.Now().Add(timeout))
	for {
		var h2 [2]byte
		if _, err := io.ReadFull(w.br, h2[:]); err != nil { return "", err }
		opcode := h2[0] & 0x0F
		l := int(h2[1] & 0x7F)
		switch l {
		case 126:
			var b [2]byte
			io.ReadFull(w.br, b[:])
			l = int(binary.BigEndian.Uint16(b[:]))
		case 127:
			var b [8]byte
			io.ReadFull(w.br, b[:])
			l = int(binary.BigEndian.Uint64(b[:]))
		}
		payload := make([]byte, l)
		if _, err := io.ReadFull(w.br, payload); err != nil { return "", err }
		switch opcode {
		case 0x1: return string(payload), nil
		case 0x8: return "", fmt.Errorf("closed")
		case 0x9: // ping → pong
			w.mu.Lock()
			pong := append([]byte{0x8A, 0x80}, []byte{0, 0, 0, 0}...)
			w.conn.Write(pong)
			w.mu.Unlock()
		}
	}
}

// ---- 业务流 ----

func authDevice(base, serverKey, devID string) (string, error) {
	req, _ := http.NewRequest("POST", base+"/v2/account/authenticate/device?create=true", strings.NewReader(`{"id":"`+devID+`"}`))
	req.Header.Set("Authorization", "Basic "+base64.StdEncoding.EncodeToString([]byte(serverKey+":")))
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	if err != nil { return "", err }
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	var m map[string]interface{}
	json.Unmarshal(b, &m)
	tok, _ := m["token"].(string)
	if tok == "" { return "", fmt.Errorf("auth failed: %s", string(b)) }
	return tok, nil
}

func bot(id int, base, wsBase, serverKey, mode string, count int, results *[]string, mu *sync.Mutex, wg *sync.WaitGroup) {
	defer wg.Done()
	prefix := fmt.Sprintf("queuebot-player-%02d", id)
	tok, err := authDevice(base, serverKey, prefix)
	if err != nil {
		fmt.Printf("bot%d auth err: %v\n", id, err)
		return
	}
	w, err := wsDial(wsBase+"?format=json&lang=go&status=&token="+tok, nil)
	if err != nil {
		fmt.Printf("bot%d ws err: %v\n", id, err)
		return
	}
	add := fmt.Sprintf(`{"matchmaker_add":{"min_count":%d,"max_count":%d,"query":"+mode:%s","string_properties":{"mode":"%s"}}}`, count, count, mode, mode)
	w.WriteText(add)

	matchID := ""
	deadline := time.Now().Add(60 * time.Second)
	dbg := 0
	for matchID == "" && time.Now().Before(deadline) {
		msg, err := w.ReadText(15 * time.Second)
		if err != nil { fmt.Printf("bot%d read err: %v\n", id, err); return }
		if dbg < 3 && id == 0 {
			dbg++
			short := msg
			if len(short) > 300 { short = short[:300] }
			fmt.Printf("bot0 RECV: %s\n", short)
		}
		var env map[string]map[string]interface{}
		json.Unmarshal([]byte(msg), &env)
		if mm, ok := env["matchmaker_matched"]; ok {
			if mid, ok2 := mm["match_id"].(string); ok2 { matchID = mid }
			if mid, ok2 := mm["matchId"].(string); ok2 { matchID = mid }
		}
	}
	if matchID == "" { fmt.Printf("bot%d no match\n", id); return }

	w.WriteText(`{"match_join":{"match_id":"` + matchID + `"}}`)
	for time.Now().Before(deadline) {
		msg, err := w.ReadText(15 * time.Second)
		if err != nil { fmt.Printf("bot%d read2 err: %v\n", id, err); return }
		if strings.Contains(msg, "match_data") {
			var env struct {
				MatchData struct {
					Op   int    `json:"op"`
					Data string `json:"data"`
				} `json:"match_data"`
			}
			json.Unmarshal([]byte(msg), &env)
			if env.MatchData.Op == 100 {
				raw, _ := base64.StdEncoding.DecodeString(env.MatchData.Data)
				var meta map[string]interface{}
				json.Unmarshal(raw, &meta)
				tl, _ := json.Marshal(meta["tokens"])
				mu.Lock()
				*results = append(*results, fmt.Sprintf("bot%d match=%s tokens=%s", id, matchID, string(tl)))
				mu.Unlock()
				fmt.Printf("bot%d GOT TOKENS match=%s\n", id, matchID)
				return
			}
		}
	}
	fmt.Printf("bot%d timeout waiting match_data\n", id)
}

func main() {
	base := flag.String("base", "http://127.0.0.1:7350", "")
	wsBase := flag.String("ws", "ws://127.0.0.1:7350/ws", "")
	serverKey := flag.String("key", "", "socket server_key")
	mode := flag.String("mode", "tdm_5v5", "对局模式")
	n := flag.Int("n", 10, "机器人数(=撮合成局人数)")
	flag.Parse()

	var mu sync.Mutex
	var results []string
	var wg sync.WaitGroup
	for i := 0; i < *n; i++ {
		wg.Add(1)
		go bot(i, *base, *wsBase, *serverKey, *mode, *n, &results, &mu, &wg)
	}
	wg.Wait()
	fmt.Printf("done bots=%d with_tokens=%d\n", *n, len(results))
}
