#pragma once

#include "CoreMinimal.h"
#include "GameServiceSubsystem.h"      // UGameServiceSubsystem（GameInstance 层服务统一基类）
#include "GameSettingsSubsystem.h"     // EGameSettingsCategory（订阅音频音量变化）
#include "GameFlowSubsystem.h"         // EGameFlowStage + FOnGameFlowStageChanged（流程驱动默认混音状态）
#include "GameAudioEventTable.h"       // FGameAudioEventEntry（查表结果的类型）
#include "GameAudioSubsystem.generated.h"

class USoundClass;   // 声音类（音量分组）
class USoundMix;     // 混音（运行期音量覆盖 / 状态切换的载体）
class USceneComponent;   // 挂点播放的挂接组件

/**
 * 竞技生化网游：音频混音状态（整段氛围的切换，不是单条音效）
 * 用途：驱动"当前该用哪套混音"（Push/Pop SoundMix）。
 */
UENUM(BlueprintType)
enum class EGameAudioState : uint8
{
    /** 启动期：尚未进入任何界面（此时通常还没有世界，混音会在世界就绪后补应用） */
    Boot     UMETA(DisplayName = "启动"),

    /** 前端：主菜单 / 房间等待 */
    Menu     UMETA(DisplayName = "前端（菜单/房间）"),

    /** 对局中 */
    Match    UMETA(DisplayName = "对局中"),

    /** 观战/旁观：由 Lua 显式切换（观战相机的听觉需求与本人视角不同） */
    Spectate UMETA(DisplayName = "观战")
};

/**
 * 竞技生化网游：音频服务（GameInstance 层服务）
 *
 * 定位：**全项目"出声"的唯一出入口**（收口），负责三件事：
 *  ① **按标签播放**：玩法/技能/UI 只报音频事件标签（GameplayTag），
 *     本服务查 `UGameAudioEventTable` 得到"用哪个声音、以什么音量/音高/衰减播"，再按 2D/挂点/位置播放；
 *     ——这是"玩法与音频资源解耦"的关键：换音效只改表，不动任何玩法代码；
 *  ② **音量设置落地**：订阅设置服务的变更，把四个音量（主/音乐/音效/语音）用
 *     `SetSoundMixClassOverride` 覆盖到四个 SoundClass 上（见 UGameAudioSettings 的分工说明）；
 *  ③ **动态混音状态**：按前端流程阶段自动切默认混音（菜单/对局），并对外提供 `SetAudioState`
 *     供 Lua 覆盖（如进入观战切 Spectate）。
 *
 * 与设置服务的分工（不越界）：
 *  - "音量是多少"由 `UGameSettingsSubsystem` 持有并持久化；
 *  - "音量怎么应用到混音"是音频实现细节，只在本服务里做；
 *  - 本服务**不读写设置数据**，只订阅广播（与本地化服务同一套纪律）。
 *
 * 装配时机（与本地化服务一致）：启动期的"订阅 + 首次应用"不在 OnServiceInitialize 里做
 * （GameInstance 层服务初始化顺序不确定），统一由 `UGameGameInstance::Init()` 调用
 * `BindSettingsAndApplyAtStartup()` 显式装配。
 *
 * 无世界时的降级策略（重要）：
 *  音量覆盖与混音 Push/Pop **都需要一个 WorldContext**（要能拿到音频设备），
 *  而启动早期可能还没有地图。因此本服务：无世界时跳过（并记录），
 *  在世界加载完成时（`FCoreUObjectDelegates::PostLoadMapWithWorld`）自动补应用一次。
 *
 * 热更新边界：本服务不含任何"声音内容"与"听感数值"——
 *  声音资源、音量倍率、音高随机、冷却、衰减全在 `UGameAudioEventTable` 与 SoundClass/SoundMix 资产里，
 *  音频同学在编辑器里改即可生效；"什么时候播"由 Lua 规则/表现决定。
 */
UCLASS()
class ZOMBIEKING_API UGameAudioSubsystem : public UGameServiceSubsystem
{
    GENERATED_BODY()

public:
    /** 服务短名（日志前缀 / 按名定位用） */
    virtual FName GetServiceName() const override;

    // -------------------------------------------------------------------------
    // 按标签播放（BlueprintCallable：供 C++ / 蓝图 / Lua 调用）
    // -------------------------------------------------------------------------
    /**
     * 在世界某处播放一条音频事件（空间化）。
     * @param EventTag         音频事件标签（须在 UGameAudioEventTable 中有条目）
     * @param Location        世界位置
     * @param InstigatorActor 触发者（可为空；提供时优先用它的世界作为播放上下文，且作为 2D 播放时的"归属者"）
     */
    UFUNCTION(BlueprintCallable, Category = "Audio")
    void PlayAudioEventAtLocation(FGameplayTag EventTag, FVector Location, AActor* InstigatorActor);

    /**
     * 把音频事件挂在某个场景组件（通常是武器/骨骼挂点）上播放（会跟随组件移动）。
     * @param AttachToComponent 挂接组件（须有效；其世界即播放上下文）
     * @param AttachSocketName  挂点名（NAME_None = 组件原点）
     */
    UFUNCTION(BlueprintCallable, Category = "Audio")
    void PlayAudioEventAttached(FGameplayTag EventTag, USceneComponent* AttachToComponent, FName AttachSocketName);

    /** 播放一条 2D 音频事件（UI 音、全局播报；不受空间衰减影响） */
    UFUNCTION(BlueprintCallable, Category = "Audio")
    void PlayAudioEvent2D(FGameplayTag EventTag);

    /**
     * 由"音频事件标签名"取标签对象（BlueprintPure，静态）。
     *
     * 为什么需要它（引擎事实，务必理解）：`FGameplayTag` 的成员函数（MatchesTag/ToString/GetTagName）
     * **都不是 UFUNCTION**，UnLua 未暴露 → Lua 无法自行"从字符串构造标签"。
     * 而 Lua 规则脚本（如对局播报"生化获胜"）又必须能表达一个标签，故提供本入口：
     *      Lua: local Tag = UE.UGameAudioSubsystem.RequestAudioEventTag("Audio.Match.ZombieWin")
     * 标签不存在时返回"无效标签"（不报错），调用方用 IsGameplayTagValid 判断即可。
     */
    UFUNCTION(BlueprintPure, Category = "Audio")
    static FGameplayTag RequestAudioEventTag(FName EventTagName);

    // -------------------------------------------------------------------------
    // 混音状态（BlueprintCallable / BlueprintPure）
    // -------------------------------------------------------------------------
    /**
     * 切换音频混音状态（幂等：状态未变时也会尝试重应用一次，用于"世界刚就绪"的补应用）。
     * 默认由前端流程自动驱动（见 HandleFlowStageChanged），Lua 可用它覆盖（例如进入观战切 Spectate）。
     */
    UFUNCTION(BlueprintCallable, Category = "Audio|State")
    void SetAudioState(EGameAudioState NewState);

    /** 当前混音状态 */
    UFUNCTION(BlueprintPure, Category = "Audio|State")
    EGameAudioState GetAudioState() const { return AudioState; }

    /** 把设置里的四个音量重新应用到混音（设置变更时自动调用；也可手动调用做强制刷新） */
    UFUNCTION(BlueprintCallable, Category = "Audio|Volume")
    void ApplyVolumeSettings();

    // -------------------------------------------------------------------------
    // 装配入口（由 UGameGameInstance::Init 在所有服务就绪后调用，见类注释）
    // -------------------------------------------------------------------------
    /**
     * 订阅设置服务（音量变更）+ 订阅前端流程（默认混音状态）+ 加载音频资产 + 首次应用音量与混音。
     * 幂等：重复调用不会重复订阅。
     */
    void BindSettingsAndApplyAtStartup();

protected:
    /** 启动：加载音频资产（表/音类/混音）并注册"世界就绪"回调（真实应用延后到有世界时） */
    virtual void OnServiceInitialize() override;

    /** 关闭：解绑全部委托、弹出已压入的混音，避免残留影响下一次启动 */
    virtual void OnServiceDeinitialize() override;

private:
    /** 设置变更回调：只处理音频分类（与"其它/全部设置已应用"），其它分类不越界 */
    UFUNCTION()
    void HandleSettingsChanged(EGameSettingsCategory Category);

    /** 前端流程阶段变化回调：把阶段机械映射为默认混音状态 */
    UFUNCTION()
    void HandleFlowStageChanged(EGameFlowStage NewStage, EGameFlowStage OldStage);

    /**
     * 世界加载完成回调（非动态委托，用 AddUObject 绑定）：
     * 音量覆盖与混音 Push/Pop 都需要世界，启动早期可能没有，故在世界就绪后补应用一次。
     */
    void HandlePostLoadMap(UWorld* LoadedWorld);

    /** 同步加载并强引用缓存全部音频资产（只尝试一次；缺失只告警不崩） */
    void EnsureAudioAssetsLoaded();

    /** 查表取条目（未命中时告警一次并返回 false） */
    bool ResolveEventEntry(FGameplayTag EventTag, FGameAudioEventEntry& OutEntry);

    /** 实际播放（三条路径统一入口）；返回是否播放成功 */
    bool PlayEventInternal(const FGameAudioEventEntry& Entry, UObject* WorldContext, USceneComponent* AttachToComponent,
                           FName AttachSocketName, const FVector& Location, AActor* InstigatorActor);

    /** 冷却闸门：同一标签在冷却期内只放行一次；未在冷却期时记录本次播放时刻并返回 true */
    bool CheckCooldown(const FGameAudioEventEntry& Entry);

    /** 取某状态对应的 SoundMix（未配置时返回空，表示该状态不施加任何混音） */
    USoundMix* ResolveStateMix(EGameAudioState State) const;

    /** 把当前状态的混音落到音频设备（幂等：先 Pop 掉上一次压入的，再 Push 目标的） */
    void ApplyAudioStateMix();

    /** 取"可用于音频播放/混音的世界上下文"（无世界时返回空，调用方据此降级） */
    UObject* GetAudioWorldContext() const;

    /** 惰性取设置服务（服务间不保证初始化先后，故一律按需取 + 判空） */
    UGameSettingsSubsystem* GetSettingsService() const;

    /** 惰性取前端流程服务 */
    UGameFlowSubsystem* GetFlowService() const;

private:
    // -------------------------------------------------------------------------
    // 资产缓存（同步加载后必须强引用，否则加载完即被 GC —— 本项目已踩过的坑）
    // -------------------------------------------------------------------------
    /** 音频事件表（全项目唯一的声音映射表） */
    UPROPERTY(Transient)
    TObjectPtr<UGameAudioEventTable> EventTable;

    /** 四个音量分组对应的 SoundClass */
    UPROPERTY(Transient)
    TObjectPtr<USoundClass> MasterSoundClass;

    UPROPERTY(Transient)
    TObjectPtr<USoundClass> MusicSoundClass;

    UPROPERTY(Transient)
    TObjectPtr<USoundClass> SFXSoundClass;

    UPROPERTY(Transient)
    TObjectPtr<USoundClass> VoiceSoundClass;

    /** 设置音量专用的 SoundMix（四个音量的运行期覆盖挂在它上面） */
    UPROPERTY(Transient)
    TObjectPtr<USoundMix> SettingsSoundMix;

    /** 各状态的混音 */
    UPROPERTY(Transient)
    TObjectPtr<USoundMix> MenuSoundMix;

    UPROPERTY(Transient)
    TObjectPtr<USoundMix> MatchSoundMix;

    UPROPERTY(Transient)
    TObjectPtr<USoundMix> SpectateSoundMix;

    // -------------------------------------------------------------------------
    // 运行状态
    // -------------------------------------------------------------------------
    /** 当前混音状态 */
    EGameAudioState AudioState = EGameAudioState::Boot;

    /**
     * 当前已压入音频设备的混音（Pop 时必须用它；为空表示当前没压任何混音）。
     * 必须跟踪的原因：Push/Pop 要成对，否则反复 Push 会在音频设备里堆叠残留（混音越切越怪）。
     */
    UPROPERTY(Transient)
    TObjectPtr<USoundMix> PushedStateMix;

    /** 各标签的最后一次播放时刻（秒，进程单调时钟）：冷却闸门用 */
    TMap<FGameplayTag, float> LastPlayTimeByTag;

    /** 已告警过的"表中缺失标签"（同一标签只告警一次，避免刷屏） */
    TSet<FGameplayTag> WarnedMissingEventTags;

    /** 是否已订阅设置服务 */
    bool bBoundToSettings = false;

    /** 是否已订阅前端流程服务 */
    bool bBoundToFlow = false;

    /** 是否已尝试加载音频资产（只尝试一次，避免每次播放都去查磁盘） */
    bool bAudioAssetsLoadAttempted = false;
};
