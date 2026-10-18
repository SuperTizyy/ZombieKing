// 拉取商城商品、开箱池、强化规则配置（只读）
// jsonb/数组列统一解码，保证客户端拿到纯 JSON

import { decodeJsonb } from "../common/api";

function decodeFields(rows: any[], fields: string[]) {
  for (let i = 0; i < rows.length; i++) {
    for (let j = 0; j < fields.length; j++) {
      rows[i][fields[j]] = decodeJsonb(rows[i][fields[j]]);
    }
  }
}

export const rpcCatalogGet: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  const items = nk.sqlQuery(
    "SELECT id, name, type, rarity, price_json::text AS price_json, mode_tags::text AS mode_tags, slot " +
    "FROM game.item_def WHERE enabled ORDER BY sort"
  );
  decodeFields(items, ["price_json"]);
  for (let i = 0; i < items.length; i++) {
    const mt = String(items[i].mode_tags || "");
    items[i].mode_tags = mt === "{}" ? [] : mt.replace(/[{}]/g, "").split(",");
  }

  const pools = nk.sqlQuery(
    "SELECT id, name, currency, cost, items_json::text AS items_json, pity::text AS pity " +
    "FROM game.gacha_pool WHERE enabled ORDER BY id"
  );
  decodeFields(pools, ["items_json", "pity"]);

  const rules = nk.sqlQuery(
    "SELECT id, target_type, from_level, to_level, success_rate, cost_json::text AS cost_json, " +
    "mats_json::text AS mats_json, fail_penalty FROM game.enhance_rule WHERE enabled " +
    "ORDER BY target_type, from_level"
  );
  decodeFields(rules, ["cost_json", "mats_json"]);

  return JSON.stringify({ items: items, pools: pools, rules: rules });
};
