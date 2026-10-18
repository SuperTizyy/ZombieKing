#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameServiceSubsystem.h" // UGameServiceSubsystem（GameInstance 层服务统一基类）
#include "GameGhostLoadoutSubsystem.generated.h"

class AGameCharacterBase;

/**
 * 竞技生化网游：幽灵角色仓库服务（"玩家拥有哪些幽灵角色"的唯一数据源）
 *
 * 玩法背景（CF 生化模式惯例，用户拍板）：
 *  - 玩家被选拔为母体 / 被攻击感染时，弹出"变身选择框"，从自己**仓库中拥有的幽灵角色**里选一个变身；
 *  - 仓库默认免费含"基础幽灵"（保底角色），保证选择流程永不卡死；
 *  - 解锁/商店/成长系统后续里程碑接入（届时新增 GrantGhost 等权威写入接口）。
 *
 * 定位与信任模型（当前阶段的明确取舍）：
 *  - 仓库真值在**客户端本地**（无后端账号体系，无法做服务器权威）；专用服务器上没有玩家的本地存档，
 *    因此对局内的防作弊校验走"客户端上报快照 → 服务器存 PlayerState → 选择 ∈ 快照"这条链
 *    （见 AGamePlayerState::SetOwnedGhostClasses 的说明）——它拦得住"凭空选任意角色类"的裸作弊，
 *    拦不住"伪造仓库清单"，后者在后端就绪后随仓库权威迁移一并解决；
 *  - 对局外（主菜单等）读取本服务展示仓库/预览；对局内选择框也读本服务（客户端侧清单来源）。
 *
 * 持久化：暂接内存默认清单（测试期全解锁）；持久化落点为玩家档案（UGameProfile），
 *         解锁系统立项时一并实现（保留 TODO 锚点，不在本轮臆造数据格式）。
 */
UCLASS(Config = Game)
class ZOMBIEKING_API UGameGhostLoadoutSubsystem : public UGameServiceSubsystem
{
    GENERATED_BODY()

public:
    /**
     * 构造函数声明（必须显式声明！）：
     * UHT 对"头文件未声明构造函数"的 UCLASS 会在 .gen.cpp 里**自动合成一个空默认构造函数体**，
     * 与本 .cpp 中定义的真实构造函数体构成双重定义（无内联宏时链接期 LNK2005，
     * 带 UE_INLINE_GENERATED_CPP_BY_NAME 时编译期 C2084）。声明后 UHT 不再合成，冲突根除。
     */
    UGameGhostLoadoutSubsystem();

    /** 服务短名（日志前缀 / 按名定位用） */
    virtual FName GetServiceName() const override { return TEXT("GhostLoadout"); }

    // -------------------------------------------------------------------------
    // 仓库查询（BlueprintPure：C++ / 蓝图 / Lua 均可调用；选择框 UI 的清单来源）
    // -------------------------------------------------------------------------
    /** 取本玩家拥有的幽灵角色清单（顺序即选择框的展示顺序，也是上报快照与索引选择的基准顺序） */
    UFUNCTION(BlueprintPure, Category = "Ghost|Loadout")
    TArray<TSubclassOf<AGameCharacterBase>> GetOwnedGhosts() const { return OwnedGhosts; }

    /** 该幽灵角色是否已拥有（空类一律视为未拥有） */
    UFUNCTION(BlueprintPure, Category = "Ghost|Loadout")
    bool IsGhostOwned(TSubclassOf<AGameCharacterBase> GhostClass) const;

    // -------------------------------------------------------------------------
    // 仓库写入（预留：解锁/商店/成长系统后续里程碑接入；当前仅测试期默认清单）
    // -------------------------------------------------------------------------
    // TODO（解锁系统里程碑）：GrantGhost / RevokeGhost + 持久化（玩家档案 UGameProfile 或后端）。

private:
    /**
     * 默认拥有的幽灵角色清单（测试期全解锁）。
     *
     * 说明：
     *  - 构造期填入三个原生角色类作为默认值；本属性标记 Config=Game，内容侧可在
     *    DefaultGame.ini 里整表覆盖（例如换成蓝图角色类路径），无需改 C++；
     *  - 顺序即选择框展示顺序，也是"上报快照 / 索引选择"的基准顺序——**改顺序 = 玩家已保存的
     *    选择索引含义变化**，因此正式运营后此表只允许追加、不允许重排（追加项放表尾）。
     */
    UPROPERTY(Config, EditDefaultsOnly, Category = "Ghost|Loadout")
    TArray<TSubclassOf<AGameCharacterBase>> OwnedGhosts;
};
