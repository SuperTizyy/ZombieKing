// ZombieKing Nakama 权威模块入口（架构文档 v1.0 §10）
//
// Nakama JS 运行时约束（v3.41.0 P1 实测）：
// 1. InitModule 必须是顶层 const InitModule = function(...)（不能箭头函数、不能 IIFE 包裹）；
// 2. 所有 initializer.registerXxx 必须直接写在 InitModule 体内，参数为顶层具名标识符/字符串。

import { beforeWriteStorageObjects, beforeDeleteStorageObjects } from "./common/storageGuard";
import { rpcPing } from "./rpc/ping";
import { rpcShopBuy } from "./rpc/shop";
import { rpcGachaDraw } from "./rpc/gacha";
import { rpcItemEnhance } from "./rpc/enhance";
import { rpcLoadoutSet } from "./rpc/loadout";
import { rpcBootstrapGet } from "./rpc/bootstrap";
import { rpcCatalogGet } from "./rpc/catalog";
import { rpcGmGrantCurrency } from "./rpc/gm";
import { rpcMatchCreate, rpcMatchSettle } from "./rpc/match";
import { matchmakerMatched, zkQueueProcHandlers, rpcQueueEnter, rpcQueueCancel, rpcQueueStatus } from "./rpc/queue";
import { zkMatchHandlers } from "./common/alloc";
import { rpcAccountBindEmail } from "./rpc/account";
import { rpcLobbyChannelGet, rpcLobbyHeartbeat, rpcOnlineList } from "./rpc/lobby";
import { rpcRoomCreate, rpcRoomJoin, rpcRoomLeave, rpcRoomList, rpcRoomStart, zkLobbyHandlers } from "./rpc/room";

const InitModule: nkruntime.InitModule = function (ctx, logger, nk, initializer) {
  initializer.registerBeforeWriteStorageObjects(beforeWriteStorageObjects);
  initializer.registerBeforeDeleteStorageObjects(beforeDeleteStorageObjects);
  initializer.registerRpc("rpc_ping", rpcPing);
  initializer.registerRpc("rpc_shop_buy", rpcShopBuy);
  initializer.registerRpc("rpc_gacha_draw", rpcGachaDraw);
  initializer.registerRpc("rpc_item_enhance", rpcItemEnhance);
  initializer.registerRpc("rpc_loadout_set", rpcLoadoutSet);
  initializer.registerRpc("rpc_bootstrap_get", rpcBootstrapGet);
  initializer.registerRpc("rpc_catalog_get", rpcCatalogGet);
  initializer.registerRpc("rpc_gm_grant_currency", rpcGmGrantCurrency);
  initializer.registerRpc("rpc_match_create", rpcMatchCreate);
  initializer.registerRpc("rpc_match_settle", rpcMatchSettle);
  initializer.registerMatchmakerMatched(matchmakerMatched);
  initializer.registerMatch("zk_match", zkMatchHandlers);
  initializer.registerMatch("zk_queueproc", zkQueueProcHandlers);
  initializer.registerRpc("rpc_queue_enter", rpcQueueEnter);
  initializer.registerRpc("rpc_queue_cancel", rpcQueueCancel);
  initializer.registerRpc("rpc_queue_status", rpcQueueStatus);
  initializer.registerRpc("rpc_account_bind_email", rpcAccountBindEmail);
  initializer.registerRpc("rpc_lobby_channel_get", rpcLobbyChannelGet);
  initializer.registerRpc("rpc_lobby_heartbeat", rpcLobbyHeartbeat);
  initializer.registerRpc("rpc_online_list", rpcOnlineList);
  initializer.registerRpc("rpc_room_create", rpcRoomCreate);
  initializer.registerRpc("rpc_room_join", rpcRoomJoin);
  initializer.registerRpc("rpc_room_leave", rpcRoomLeave);
  initializer.registerRpc("rpc_room_list", rpcRoomList);
  initializer.registerRpc("rpc_room_start", rpcRoomStart);
  initializer.registerMatch("zk_lobby", zkLobbyHandlers);
  logger.info("ZombieKing modules loaded (P6-lobby)");
};
