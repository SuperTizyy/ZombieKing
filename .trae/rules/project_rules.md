# ZombieKing 工作区总规则（路由 + 全局硬约束）

工作区根下包含两个子工程，规则**分文件管理、由本文件路由**：

| 子工程 | 内容 | 专属规则文件 |
|---|---|---|
| `ZombieKing/` | UE5.7 游戏客户端/DS（UnLua） | `ZombieKing\.trae\rules\project_rules.md` |
| `ZombieKingServer/` | Nakama + TypeScript + PostgreSQL + Go 服务端 | `ZombieKingServer\.trae\rules\project_rules.md` |

## 规则加载（每次会话必须遵守）

- 修改 `ZombieKing/` 下任何代码/配置前，**必须先读取** `ZombieKing\.trae\rules\project_rules.md`。
- 修改 `ZombieKingServer/` 下任何代码/配置前，**必须先读取** `ZombieKingServer\.trae\rules\project_rules.md`。
- 跨两端任务（协议对接、契约变更）需**同时读取两份**专属规则。
- 专属规则与本文件冲突时，以专属规则中"仅适用本端"的条款为准。

## 全局硬约束（两端共同遵守）

1. **注释**：关键代码必须写详细中文注释，说明该行/该块的**目的**与**功能**，而不是复述语法。
2. **决策**：遇到架构取向、命名规范、模块拆分等需要权衡的问题，**先问用户**——给出选项与推荐（带理由），等用户确认后再动手。
3. **整理数据时**有拿不定主意的地方，向用户提问确认，不擅自拍板。

## UE 编译红线（仅约束 `ZombieKing/` 子工程）

- AI **严禁**运行任何 UE 编译/构建命令（`Build`、`msbuild`、`UnrealBuildTool`/`UBT`、`GenerateProjectFiles`、`RunUAT`、`cl`、`dotnet build`）；编译、生成工程、启动编辑器均由用户手动完成，AI 只改源码与配置。
- 服务端侧的 `npm run build`（esbuild）、`go build`、脚本与测试**不受此红线限制**，按服务端规则文件执行。

## 共享契约锚点（改动需双端同步）

- 架构设计文档 v1.0（评审通过基线）：项目记忆目录 `architecture-design.md`；**任何架构级变更先更新该文档再改代码**。
- DS 启动参数约定：`-log -port={port} -matchId={match_id} -dsId={ds_id} -dsSecret={ds_secret}`。
- join_token / 结算 HMAC canonical / RPC 契约清单：以服务端规则文件（含"UE 客户端接入 P8"节）为权威；UE 侧接入实现当前未挂接（P8 改动已从工程移除），重做时按该节契约恢复并保持双端同源。
