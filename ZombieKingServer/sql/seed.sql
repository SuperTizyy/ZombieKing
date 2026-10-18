-- ZombieKing 配置种子数据（幂等：ON CONFLICT DO NOTHING）
-- 商品/开箱池/强化规则，依据架构文档 v1.0 §12-3

INSERT INTO game.currency_def (code, name, cap) VALUES
    ('gold',    '金币',   0),
    ('diamond', '钻石',   0),
    ('token',   '活动代币', 99999)
ON CONFLICT (code) DO NOTHING;

INSERT INTO game.item_def (id, name, type, rarity, price_json, mode_tags, slot, sort) VALUES
    -- 主武器
    ('wp_rifle_ak',      'AK-47 标准型',   'weapon', 0, '{"gold":8000}',          '{tdm_5v5,zombie_infection}', 'primary',   10),
    ('wp_smg_ump',       'UMP 冲锋枪',     'weapon', 0, '{"gold":5000}',          '{tdm_5v5,zombie_infection}', 'primary',   11),
    ('wp_sniper_m200',   'M200 狙击枪',    'weapon', 1, '{"gold":25000}',         '{tdm_5v5}',                  'primary',   12),
    ('wp_shotgun_m3',    'M3 霰弹枪',      'weapon', 1, '{"gold":18000}',         '{zombie_infection}',         'primary',   13),
    -- 副武器
    ('wp_pistol_glock',  'Glock-18',       'weapon', 0, '{"gold":2000}',          '{tdm_5v5,zombie_infection}', 'secondary', 20),
    ('wp_pistol_1911',   'M1911',          'weapon', 0, '{"gold":2500}',          '{tdm_5v5,zombie_infection}', 'secondary', 21),
    -- 近战
    ('wp_melee_knife',   '军刀',           'weapon', 0, '{"gold":1000}',          '{tdm_5v5,zombie_infection}', 'melee',     30),
    -- 皮肤
    ('sk_ak_crimson',    'AK-47 血色黎明', 'skin',   2, '{"diamond":800}',        '{tdm_5v5,zombie_infection}', 'skin_primary', 40),
    ('sk_m200_dragon',   'M200 苍龙',      'skin',   3, '{"diamond":3000}',       '{tdm_5v5}',                  'skin_primary', 41),
    -- 材料
    ('enhance_stone',    '强化石',         'material', 0, '{"gold":500}',         '{}',                         '',          90),
    ('enhance_core',     '高级强化核心',   'material', 1, '{"diamond":50}',       '{}',                         '',          91)
ON CONFLICT (id) DO NOTHING;

INSERT INTO game.gacha_pool (id, name, currency, cost, items_json, pity) VALUES
    ('standard_pool', '标准补给箱', 'diamond', 10,
     '[
        {"item_def_id":"wp_sniper_m200",   "weight":3,  "rarity":1},
        {"item_def_id":"wp_shotgun_m3",    "weight":7,  "rarity":1},
        {"item_def_id":"sk_ak_crimson",    "weight":5,  "rarity":2},
        {"item_def_id":"sk_m200_dragon",   "weight":1,  "rarity":3},
        {"item_def_id":"enhance_stone",    "weight":40, "rarity":0},
        {"item_def_id":"enhance_core",     "weight":14, "rarity":1},
        {"item_def_id":"wp_pistol_1911",   "weight":30, "rarity":0}
      ]'::jsonb,
     '{"legendary_at":50, "epic_at":10}'::jsonb),
    ('token_pool', '活动代币箱', 'token', 1,
     '[
        {"item_def_id":"wp_melee_knife", "weight":20, "rarity":0},
        {"item_def_id":"enhance_stone",  "weight":60, "rarity":0},
        {"item_def_id":"sk_ak_crimson",  "weight":5,  "rarity":2},
        {"item_def_id":"wp_pistol_glock","weight":15, "rarity":0}
      ]'::jsonb,
     '{"epic_at":20}'::jsonb)
ON CONFLICT (id) DO NOTHING;

INSERT INTO game.enhance_rule (id, target_type, from_level, to_level, success_rate, cost_json, mats_json, fail_penalty) VALUES
    ('enh_w_0_1', 'weapon', 0, 1, 0.95000, '{"currency":"gold","amount":1000}',  '[{"item_def_id":"enhance_stone","count":1}]', 'keep'),
    ('enh_w_1_2', 'weapon', 1, 2, 0.80000, '{"currency":"gold","amount":2000}',  '[{"item_def_id":"enhance_stone","count":2}]', 'keep'),
    ('enh_w_2_3', 'weapon', 2, 3, 0.60000, '{"currency":"gold","amount":4000}',  '[{"item_def_id":"enhance_stone","count":4}]', 'downgrade'),
    ('enh_w_3_4', 'weapon', 3, 4, 0.40000, '{"currency":"gold","amount":8000}',  '[{"item_def_id":"enhance_core","count":1}]',  'downgrade'),
    ('enh_w_4_5', 'weapon', 4, 5, 0.20000, '{"currency":"gold","amount":15000}', '[{"item_def_id":"enhance_core","count":3}]',  'downgrade')
ON CONFLICT (id) DO NOTHING;
