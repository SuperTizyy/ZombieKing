// ZombieKing 压测工具（架构文档 §11 P5：百级 CCU）
// 纯 stdlib。每 worker 模拟一个设备用户循环：auth → bootstrap/catalog 读 → shop 写(含幂等重放)
// 输出吞吐与 p50/p95/p99。业务 4xx（如 INSUFFICIENT_FUNDS）计入 bizErr，不计入失败。
package main

import (
	"bytes"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

var (
	totalCalls int64
	bizErrs    int64
	transErrs int64
	latMu      sync.Mutex
	latencies  []int64 // ms
)

var client = &http.Client{
	Timeout: 15 * time.Second,
	Transport: &http.Transport{
		MaxIdleConns:        512,
		MaxIdleConnsPerHost: 256,
		MaxConnsPerHost:     400,
	},
}

func uuidv4() string {
	b := make([]byte, 16)
	rand.Read(b)
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}

func recordLat(ms int64, biz bool, trans bool) {
	atomic.AddInt64(&totalCalls, 1)
	if biz { atomic.AddInt64(&bizErrs, 1) }
	if trans { atomic.AddInt64(&transErrs, 1) }
	latMu.Lock(); latencies = append(latencies, ms); latMu.Unlock()
}

func call(base, tok, basic, rpc, body string) {
	start := time.Now()
	var req *http.Request
	if rpc == "" { // device auth
		req, _ = http.NewRequest("POST", base+"/v2/account/authenticate/device?create=true", strings.NewReader(`{"id":"`+body+`"}`))
		req.Header.Set("Authorization", basic)
	} else {
		req, _ = http.NewRequest("POST", base+"/v2/rpc/"+rpc, strings.NewReader(body))
		req.Header.Set("Authorization", "Bearer "+tok)
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := client.Do(req)
	ms := time.Since(start).Milliseconds()
	if err != nil {
		recordLat(ms, false, true)
		return
	}
	io.Copy(io.Discard, io.LimitReader(resp.Body, 1<<16))
	resp.Body.Close()
	biz := resp.StatusCode >= 400
	recordLat(ms, biz, false)
}

func worker(base, basic string, deadline time.Time) {
	devID := uuidv4()
	start := time.Now()
	httpReq, _ := http.NewRequest("POST", base+"/v2/account/authenticate/device?create=true", bytes.NewReader([]byte(`{"id":"`+devID+`"}`)))
	httpReq.Header.Set("Authorization", basic)
	httpReq.Header.Set("Content-Type", "application/json")
	resp, err := client.Do(httpReq)
	if err != nil {
		recordLat(time.Since(start).Milliseconds(), false, true)
		return
	}
	b, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<16))
	resp.Body.Close()
	var m map[string]interface{}
	json.Unmarshal(b, &m)
	tok, _ := m["token"].(string)
	recordLat(time.Since(start).Milliseconds(), resp.StatusCode >= 400, false)
	if tok == "" { return }

	for time.Now().Before(deadline) {
		switch atomic.AddInt64(&totalCalls, 0) % 5 {
		case 0, 1:
			call(base, tok, basic, "rpc_bootstrap_get", `"{}"`)
		case 2:
			call(base, tok, basic, "rpc_catalog_get", `"{}"`)
		case 3:
			idem := uuidv4()
			call(base, tok, basic, "rpc_shop_buy", `"{\"item_def_id\":\"enhance_stone\",\"qty\":1,\"currency\":\"gold\",\"idempotency_key\":\"`+idem+`\"}"`)
			// 幂等重放
			call(base, tok, basic, "rpc_shop_buy", `"{\"item_def_id\":\"enhance_stone\",\"qty\":1,\"currency\":\"gold\",\"idempotency_key\":\"`+idem+`\"}"`)
		default:
			call(base, tok, basic, "rpc_ping", `"{}"`)
		}
		time.Sleep(50 * time.Millisecond) // 模拟玩家操作间隔(think time)
	}
}

func main() {
	users := flag.Int("users", 100, "并发用户数")
	dur := flag.Duration("dur", 20*time.Second, "压测时长")
	base := flag.String("base", "http://127.0.0.1:7350", "Nakama 地址")
	serverKey := flag.String("key", "", "socket server_key (Basic 认证)")
	flag.Parse()
	if *serverKey == "" { panic("需要 -key") }
	basic := "Basic " + base64.StdEncoding.EncodeToString([]byte(*serverKey+":"))

	deadline := time.Now().Add(*dur)
	start := time.Now()
	var wg sync.WaitGroup
	for i := 0; i < *users; i++ {
		wg.Add(1)
		go func() { defer wg.Done(); worker(*base, basic, deadline) }()
	}
	wg.Wait()

	latMu.Lock()
	sort.Slice(latencies, func(a, b int) bool { return latencies[a] < latencies[b] })
	p := func(q float64) int64 {
		if len(latencies) == 0 { return 0 }
		i := int(float64(len(latencies)-1) * q)
		return latencies[i]
	}
	elapsed := time.Since(start).Seconds()
	var maxMs int64
	if len(latencies) > 0 { maxMs = latencies[len(latencies)-1] }
	fmt.Printf("users=%d dur=%.0fs calls=%d rps=%.1f bizErr=%d transErr=%d p50=%dms p95=%dms p99=%dms max=%dms\n",
		*users, elapsed, totalCalls, float64(totalCalls)/elapsed, bizErrs, transErrs,
		p(0.50), p(0.95), p(0.99), maxMs)
	latMu.Unlock()
}
