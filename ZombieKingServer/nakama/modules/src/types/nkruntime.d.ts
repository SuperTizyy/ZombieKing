// Nakama 服务端 JS 运行时类型 shim
// 说明：npm 上不存在官方 @heroiclabs/nakama-runtime / nakama-common 包（2026-10 已验证 404），
// 故按 nakama.exe v3.41.0 内嵌 goja 运行时的实际 API 维护本地最小类型声明，随用随扩。

declare namespace nkruntime {
  interface Context {
    userId?: string;
    username?: string;
    sessionId?: string;
    environment?: { [key: string]: string };
    userMetadata?: { [key: string]: unknown };
  }

  interface Session {
    userId: string;
    username: string;
    vars?: { [key: string]: string };
  }

  interface Logger {
    debug(msg: string, ...fields: unknown[]): void;
    info(msg: string, ...fields: unknown[]): void;
    warn(msg: string, ...fields: unknown[]): void;
    error(msg: string, ...fields: unknown[]): void;
  }

  interface StorageWrite {
    collection: string;
    key: string;
    userId?: string;
    username?: string;
    value: { [key: string]: unknown };
    version?: string;
    permissionRead?: number;
    permissionWrite?: number;
  }

  interface StorageObject {
    collection: string;
    key: string;
    userId: string;
    username: string;
    version: string;
    value: { [key: string]: unknown };
    permissionRead: number;
    permissionWrite: number;
    createTime: number;
    updateTime: number;
  }

  interface StorageDelete {
    collection: string;
    key: string;
    userId?: string;
    username?: string;
    version?: string;
  }



  interface Presence { userId: string; sessionId: string; username: string; node: string; }

  interface MatchmakerEntry {
    presence: Presence;
    properties: { [key: string]: any };
    partyId?: string;
  }

  interface MatchDispatcher {
    /** v3.41 实测签名：(opCode, data, presences, sender, reliable)——sender 第 4、reliable 第 5（与文档顺序不同） */
    broadcastMessage(op: number, data: string, presences?: Presence[] | null, sender?: Presence | null, reliable?: boolean): void;
  }

  type MatchmakerMatchedFunction = (ctx: Context, logger: Logger, nk: Nakama, entries: MatchmakerEntry[]) => string | null;
  type MatchInitFunction<S> = (ctx: Context, logger: Logger, nk: Nakama, params: any) => { state: S; tickRate: number; label?: string };
  type MatchJoinAttemptFunction<S> = (ctx: Context, logger: Logger, nk: Nakama, dispatcher: MatchDispatcher, tick: number, state: S, presence: Presence, metadata: any) => { state: S; accept: boolean } | null;
  type MatchJoinFunction<S> = (ctx: Context, logger: Logger, nk: Nakama, dispatcher: MatchDispatcher, tick: number, state: S, presences: Presence[]) => { state: S } | null;
  type MatchLeaveFunction<S> = (ctx: Context, logger: Logger, nk: Nakama, dispatcher: MatchDispatcher, tick: number, state: S, presences: Presence[]) => { state: S } | null;
  type MatchLoopFunction<S> = (ctx: Context, logger: Logger, nk: Nakama, dispatcher: MatchDispatcher, tick: number, state: S, messages: any[]) => { state: S } | null;
  type MatchTerminateFunction<S> = (ctx: Context, logger: Logger, nk: Nakama, dispatcher: MatchDispatcher, tick: number, graceSeconds: number) => S | null;
  type MatchSignalFunction<S> = (ctx: Context, logger: Logger, nk: Nakama, dispatcher: MatchDispatcher, tick: number, state: S, data: string) => { state: S; data?: string } | null;

  interface MatchHandler {
    matchInit: MatchInitFunction<any>;
    matchJoinAttempt: MatchJoinAttemptFunction<any>;
    matchJoin: MatchJoinFunction<any>;
    matchLeave: MatchLeaveFunction<any>;
    matchLoop: MatchLoopFunction<any>;
    matchTerminate: MatchTerminateFunction<any>;
    matchSignal: MatchSignalFunction<any>;
  }

  interface Group {
    group: { id: string; create_time: number; update_time: number };
    creator: { user_id: string; username: string };
    name: string;
    description: string;
    open_visibility: boolean;
    editable: boolean;
    max_count: number;
    state: number;
    avatar_url: string;
    lang_code: string;
    members: any[];
  }

  interface HttpResponse { code: number; headers: any; body: string; }

  interface Nakama {
    /** v3.41.0 实测：返回行对象数组（按列名取值）；多语句事务见 PL/pgSQL 函数约定（项目规则） */
    sqlQuery<T = { [column: string]: any }>(query: string, params?: unknown[]): T[];
    /** v3.41.0 实测：同步 HTTP（源码 runtime_javascript_nakama.go 已验证） */
    httpRequest(url: string, method: string, headers: { [k: string]: string }, body: string, timeoutMs?: number): HttpResponse;
    matchCreate(name: string, params: any): string;
    matchGet(matchId: string): any;
    /** 服务端内部写 Storage，不触发 before 钩子 */
    storageWrite(userId: string, objects: StorageWrite[]): StorageObject[];
    storageRead(userId: string, reads: { collection: string; key: string; userId?: string }[][]): StorageObject[];
    /** 将邮箱+密码绑定到已有账号（设备账号升级用）；失败 throw */
    linkEmail(userId: string, email: string, password: string): void;
    /** 创建群组；openVisibility=true 为公开组 */
    groupCreate(name: string, description: string, avatarUrl: string, langCode: string, openVisibility: boolean, maxCount: number): Group;
    /** 运行时安全随机（开箱/强化掷点用） */
    rand(min: number, max: number): number;
    uuidv4(): string;
    /** 返回 ArrayBuffer(goja)，需自行转 hex（见 common/hmac.ts） */
    hmacSha256Hash(input: string, key: string): any;
    sha256Hash(input: string): any;
  }

  // 实测 v3.41.0 goja 运行时 RPC 调用约定为 (ctx, logger, nk, params)
  type RpcFunction = (
    ctx: Context,
    logger: Logger,
    nk: Nakama,
    params: string
  ) => string;

  // v3.41.0 实测钩子约定：(ctx, logger, nk, input)，input 为包装对象；throw 拒绝，返回 undefined/null 放行
  interface BeforeWriteStorageInput { objects: StorageWrite[]; }
  interface BeforeDeleteStorageInput { objectIds: StorageDelete[]; }

  type BeforeWriteStorageObjectsFunction = (
    ctx: Context,
    logger: Logger,
    nk: Nakama,
    input: BeforeWriteStorageInput
  ) => BeforeWriteStorageInput | void;

  type BeforeDeleteStorageObjectsFunction = (
    ctx: Context,
    logger: Logger,
    nk: Nakama,
    input: BeforeDeleteStorageInput
  ) => void | null;

  interface Initializer {
    registerRpc(id: string, func: RpcFunction): void;
    registerMatchmakerMatched(func: MatchmakerMatchedFunction): void;
    registerMatch(name: string, handlers: MatchHandler): void;
    registerBeforeWriteStorageObjects(func: BeforeWriteStorageObjectsFunction): void;
    registerBeforeDeleteStorageObjects(func: BeforeDeleteStorageObjectsFunction): void;
    registerAfter(fn: (ctx: Context, logger: Logger, nk: Nakama, session: Session) => void): void;
  }

  type InitModule = (ctx: Context, logger: Logger, nk: Nakama, initializer: Initializer) => void;
}
