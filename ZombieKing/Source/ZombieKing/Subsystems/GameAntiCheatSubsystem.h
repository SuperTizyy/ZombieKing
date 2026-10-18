#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"   // UWorldSubsystem（本类基类：生命周期绑定"世界=一场对局"）
#include "GameAntiCheatSubsystem.generated.h"

/**
 * 可疑行为的类别（用于分类统计与分级处置）。
 * 说明：分类只描述"哪一环出问题"，不携带任何处置规则——"封禁/踢出/仅记录"由后端与 Lua 规则决定。
 */
UENUM(BlueprintType)
enum class EGameCheatCategory : uint8
{
    /** 移动异常：速度/位移超出合法范围、非法爬墙等 */
    Movement        UMETA(DisplayName = "移动异常"),
    /** 瞄准异常：视线/命中角度在物理上不可能（如背后甩狙、穿墙命中） */
    Aim             UMETA(DisplayName = "瞄准异常"),
    /** 伤害异常：伤害来源与武器/距离不匹配、异常高频攻击 */
    Damage          UMETA(DisplayName = "伤害异常"),
    /** 客户端完整性：内存/资源被篡改、非法指令包 */
    ClientIntegrity UMETA(DisplayName = "客户端异常"),
    /** 其它未归类可疑行为 */
    Other           UMETA(DisplayName = "其它")
};

/**
 * 一条可疑行为记录（世界内存态，不复制）。
 * 只保存稳定标识（PlayerId）而非指针：避免记录长期持有 PlayerController/PlayerState 引用而阻止 GC。
 */
USTRUCT(BlueprintType)
struct FGameCheatReport
{
    GENERATED_BODY()

    /** 被举报玩家的 PlayerId（来自 APlayerState::GetPlayerId，全生命周期稳定、可用于后端对账） */
    UPROPERTY(BlueprintReadOnly, Category = "AntiCheat")
    int32 PlayerId = INDEX_NONE;

    /** 可疑行为类别 */
    UPROPERTY(BlueprintReadOnly, Category = "AntiCheat")
    EGameCheatCategory Category = EGameCheatCategory::Other;

    /** 细节描述（自由文本/JSON：为排障与申诉保留原始证据线索） */
    UPROPERTY(BlueprintReadOnly, Category = "AntiCheat")
    FString Details;

    /** 记录时的服务器时间（秒），用于与对局录像/回放对齐 */
    UPROPERTY(BlueprintReadOnly, Category = "AntiCheat")
    double ServerTimeSeconds = 0.0;
};

/** 可疑行为上报广播（供 Lua 规则脚本 / 后端上报模块 / 管理员工具订阅） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameCheatReported, const FGameCheatReport&, Report);

/**
 * 竞技生化网游：反作弊服务（World 层服务）
 *
 * 为什么是 WorldSubsystem 而不是 GameInstanceSubsystem：
 *  - 反作弊的取证与判定都以"一场对局（一个世界）"为边界，世界销毁即应停止并清空记录；
 *    WorldSubsystem 的生命周期恰好与之对齐（每张地图各一个实例，PIE 下也按世界隔离）；
 *  - 也正因生命周期不同，本类**不继承** UGameServiceSubsystem（C++ 单继承，二者无继承关系），
 *    与 GameInstance 层服务并列存在，各自遵循"机制在 C++、规则在 Lua"的同一纪律。
 *
 * 职责边界：
 *  - 本服务只做"机制"：接收可疑行为上报 → 打时间戳 → 归档 → 标记玩家 → 广播；
 *  - 本服务**不做最终处置**：是否踢出/封禁由服务端安全模块与 Lua 规则决定（处置策略需要可热更）；
 *  - 校验逻辑（从哪里产生可疑行为）由各系统自行调用上报接口，例如：
 *      · 命中合法性校验（UCombatComponent 的 ValidateIncomingHit）失败时上报 Aim/Damage；
 *      · 移动合法性校验（后续里程碑：结合 Mover 的服务器重模拟结果）失败时上报 Movement。
 *
 * TODO（后续里程碑）：
 *  - 移动层校验尚未实现（当前只有命中校验）；接入后在此汇聚两类证据；
 *  - 证据上报后端通道未接入，当前仅本地归档 + 广播（与遥测服务同样先落地"机制"，后接后端）。
 */
UCLASS()
class ZOMBIEKING_API UGameAntiCheatSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    // -------------------------------------------------------------------------
    // 生命周期（世界级）
    // -------------------------------------------------------------------------
    /** 世界创建并初始化时调用：复位世界内的取证状态 */
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    /** 世界销毁时调用：清空全部记录，避免跨世界残留（WorldSubsystem 随世界销毁，此处只做内存释放） */
    virtual void Deinitialize() override;

    // -------------------------------------------------------------------------
    // 上报入口（BlueprintCallable：供 C++ 校验逻辑与 Lua 规则统一调用）
    // -------------------------------------------------------------------------
    /**
     * 上报一次可疑行为（服务器权威；本函数只归档与广播，不做处置）。
     * @param PlayerId 被举报玩家的 PlayerId（来自 APlayerState::GetPlayerId）
     * @param Category 可疑行为类别
     * @param Details  细节描述（自由文本/JSON）
     */
    UFUNCTION(BlueprintCallable, Category = "AntiCheat")
    void ReportCheat(int32 PlayerId, EGameCheatCategory Category, const FString& Details);

    // -------------------------------------------------------------------------
    // 查询
    // -------------------------------------------------------------------------
    /** 该玩家是否已被标记（表现层可据此在观战/回放中做标记；最终处置仍由后端决定） */
    UFUNCTION(BlueprintPure, Category = "AntiCheat")
    bool IsPlayerFlagged(int32 PlayerId) const;

    /** 取本世界已归档的全部可疑行为记录 */
    UFUNCTION(BlueprintPure, Category = "AntiCheat")
    const TArray<FGameCheatReport>& GetCheatReports() const { return Reports; }

    // -------------------------------------------------------------------------
    // 广播（BlueprintAssignable：Lua / 后端上报模块订阅）
    // -------------------------------------------------------------------------
    /** 新增一条可疑行为记录时广播 */
    UPROPERTY(BlueprintAssignable, Category = "AntiCheat")
    FOnGameCheatReported OnCheatReported;

private:
    /** 本世界的可疑行为归档（内存态；世界销毁即释放） */
    UPROPERTY(Transient)
    TArray<FGameCheatReport> Reports;

    /** 已被标记的玩家 Id（unordered set，O(1) 查询；仅存稳定标识，不持指针） */
    TSet<int32> FlaggedPlayerIds;
};
