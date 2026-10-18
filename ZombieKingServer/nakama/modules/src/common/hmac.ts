// HMAC/字节工具：nk.hmacSha256Hash 返回 ArrayBuffer(goja)，统一转 hex 比较
// canonical 串约定（跨语言可复现，PowerShell 假 DS 同样实现）：
//   canon = match_id|ds_id|settled_at|payload_hash

export function bytesToHex(buf: any): string {
  let view = buf;
  if (typeof ArrayBuffer !== "undefined" && buf instanceof ArrayBuffer) {
    view = new Uint8Array(buf);
  }
  let s = "";
  const len = view.length !== undefined ? view.length : view.byteLength;
  for (let i = 0; i < len; i++) {
    const b = view[i];
    s += (b < 16 ? "0" : "") + b.toString(16);
  }
  return s;
}

export function hmacHex(nk: nkruntime.Nakama, input: string, key: string): string {
  return bytesToHex((nk as any).hmacSha256Hash(input, key));
}

export function settleCanon(matchId: string, dsId: string, settledAt: number, payloadHash: string): string {
  return matchId + "|" + dsId + "|" + settledAt + "|" + payloadHash;
}

export function verifySettleSig(nk: nkruntime.Nakama, p: any, dsSecret: string): boolean {
  const canon = settleCanon(String(p.match_id), String(p.ds_id), Number(p.settled_at), String(p.payload_hash));
  const expect = hmacHex(nk, canon, dsSecret);
  const got = String(p.sig || "");
  if (expect.length !== got.length) { return false; }
  let diff = 0;
  for (let i = 0; i < expect.length; i++) {
    diff |= expect.charCodeAt(i) ^ got.charCodeAt(i);
  }
  return diff === 0;
}
