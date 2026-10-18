#include "GameAudioSubsystem.h"

#include "GameAudioSettings.h"            // UGameAudioSettings（事件表 / 四个 SoundClass / 各状态 SoundMix 的路径）
#include "Engine/GameInstance.h"          // UGameInstance::GetWorld / GetSubsystem（GameInstance 层服务没有 GetWorld）
#include "Engine/World.h"                 // UWorld
#include "Kismet/GameplayStatics.h"       // PlaySound2D / SpawnSoundAtLocation / SpawnSoundAttached / SoundMix 系列
#include "Sound/SoundBase.h"              // USoundBase（声音资产）
#include "Sound/SoundClass.h"             // USoundClass（音量分组）
#include "Sound/SoundMix.h"               // USoundMix（音量覆盖 / 状态混音）
#include "Sound/SoundAttenuation.h"       // USoundAttenuation（空间衰减）
#include "Components/SceneComponent.h"    // USceneComponent（挂点播放的挂接组件）
#include "GameFramework/Actor.h"          // AActor（Instigator / GetNameSafe）
#include "UObject/UObjectGlobals.h"       // FCoreUObjectDelegates::PostLoadMapWithWorld
#include "HAL/PlatformTime.h"             // FPlatformTime::Seconds（冷却闸门用的进程单调时钟）
#include "ZombieKing.h"                   // LogZombieKing

// =============================================================================
//  服务标识
// =============================================================================

FName UGameAudioSubsystem::GetServiceName() const
{
    // 稳定短名：日志前缀与按名定位均依赖它，一经发布不得随意变更
    return FName(TEXT("Audio"));
}

FGameplayTag UGameAudioSubsystem::RequestAudioEventTag(FName EventTagName)
{
    // 走引擎标签管理器按名字取标签；不存在时返回无效标签而非报错 ——
    // 调用方（尤其 Lua 规则脚本）用 IsGameplayTagValid 判断即可，避免因一个笔误就抛异常。
    return FGameplayTag::RequestGameplayTag(EventTagName, /*ErrorIfNotFound=*/false);
}

// =============================================================================
//  生命周期
// =============================================================================

void UGameAudioSubsystem::OnServiceInitialize()
{
    // 加载全部音频资产（表 / 四个 SoundClass / 各状态与设置用的 SoundMix）
    EnsureAudioAssetsLoaded();

    // 注册"世界就绪"回调：
    // 音量覆盖与混音 Push/Pop 都必须有世界上下文（要能拿到音频设备），而启动早期可能还没有地图；
    // 因此这里注册引擎的地图加载完成消息，在世界就绪后自动补应用一次（本服务唯一依赖引擎回调的地方）。
    FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UGameAudioSubsystem::HandlePostLoadMap);
}

void UGameAudioSubsystem::OnServiceDeinitialize()
{
    // 解绑"世界就绪"回调（非动态委托用 RemoveAll）
    FCoreUObjectDelegates::PostLoadMapWithWorld.RemoveAll(this);

    // 解绑设置服务订阅
    if (bBoundToSettings)
    {
        if (UGameSettingsSubsystem* SettingsService = GetSettingsService())
        {
            SettingsService->OnSettingsChanged.RemoveDynamic(this, &UGameAudioSubsystem::HandleSettingsChanged);
        }
        bBoundToSettings = false;
    }

    // 解绑前端流程订阅
    if (bBoundToFlow)
    {
        if (UGameFlowSubsystem* FlowService = GetFlowService())
        {
            FlowService->OnFlowStageChanged.RemoveDynamic(this, &UGameAudioSubsystem::HandleFlowStageChanged);
        }
        bBoundToFlow = false;
    }

    // 尽力弹出已压入的混音：关闭后不得留下混音残留（否则会污染下一次启动的听感）。
    // 此时可能已无世界，故只做"能做的那部分"，失败也不报错。
    if (PushedStateMix)
    {
        if (UObject* WorldContext = GetAudioWorldContext())
        {
            UGameplayStatics::PopSoundMixModifier(WorldContext, PushedStateMix);
        }
        PushedStateMix = nullptr;
    }

    LastPlayTimeByTag.Reset();
    WarnedMissingEventTags.Reset();
}

// =============================================================================
//  装配（由 UGameGameInstance::Init 调用）
// =============================================================================

void UGameAudioSubsystem::BindSettingsAndApplyAtStartup()
{
    // -------------------------------------------------------------------------
    // ① 订阅设置服务：音量变更 → 重新应用（与本地化服务同一套"收口"纪律）
    // -------------------------------------------------------------------------
    if (!bBoundToSettings)
    {
        if (UGameSettingsSubsystem* SettingsService = GetSettingsService())
        {
            SettingsService->OnSettingsChanged.AddDynamic(this, &UGameAudioSubsystem::HandleSettingsChanged);
            bBoundToSettings = true;
        }
        else
        {
            // 设置服务不可用属异常（同一 GameInstance 上必然存在）：只告警不中断
            UE_LOG(LogZombieKing, Error, TEXT("[音频] 设置服务不可用，音量变更将无法自动应用"));
        }
    }

    // -------------------------------------------------------------------------
    // ② 订阅前端流程：默认混音状态随阶段自动切换
    // -------------------------------------------------------------------------
    if (!bBoundToFlow)
    {
        if (UGameFlowSubsystem* FlowService = GetFlowService())
        {
            FlowService->OnFlowStageChanged.AddDynamic(this, &UGameAudioSubsystem::HandleFlowStageChanged);
            bBoundToFlow = true;

            // 订阅后立即按"当前阶段"同步一次：若在订阅之前流程已经切到某阶段（例如启动直接进对局），
            // 不同步就会漏掉那一次状态切换，导致混音停留在启动态。
            const EGameFlowStage CurrentStage = FlowService->GetFlowStage();
            HandleFlowStageChanged(CurrentStage, CurrentStage);
        }
        else
        {
            UE_LOG(LogZombieKing, Error, TEXT("[音频] 前端流程服务不可用，混音状态将不会随流程自动切换"));
        }
    }

    // -------------------------------------------------------------------------
    // ③ 首次应用（无世界时这两个函数内部会自动跳过，等 PostLoadMapWithWorld 补应用）
    // -------------------------------------------------------------------------
    EnsureAudioAssetsLoaded();
    ApplyVolumeSettings();
    ApplyAudioStateMix();
}

// =============================================================================
//  事件回调
// =============================================================================

void UGameAudioSubsystem::HandleSettingsChanged(EGameSettingsCategory Category)
{
    // 音频分类变化 → 重新应用音量。
    // "Other" 表示设置服务做了整体应用（ApplyAndSaveSettings）——玩家点"应用"时也应重刷音量，故一并处理。
    if (Category == EGameSettingsCategory::Audio || Category == EGameSettingsCategory::Other)
    {
        ApplyVolumeSettings();
    }
}

void UGameAudioSubsystem::HandleFlowStageChanged(EGameFlowStage NewStage, EGameFlowStage OldStage)
{
    // 机械映射（这是"默认行为"，不是玩法规则）：前端（主菜单/房间）→ 菜单混音；进入对局 → 对局混音。
    // 观战等更细的听觉状态由 Lua 显式调用 SetAudioState（例如进入观战切 Spectate）——
    // 换句话说：C++ 只提供"合理的默认"，规则仍由 Lua 决定（符合本项目"规则下沉"纪律）。
    switch (NewStage)
    {
    case EGameFlowStage::Match:
        SetAudioState(EGameAudioState::Match);
        break;

    case EGameFlowStage::MainMenu:
    case EGameFlowStage::Room:
        SetAudioState(EGameAudioState::Menu);
        break;

    case EGameFlowStage::Boot:
    default:
        // 启动阶段保持现状：此时通常还没有世界，混音会在世界就绪后补应用
        break;
    }
}

void UGameAudioSubsystem::HandlePostLoadMap(UWorld* LoadedWorld)
{
    // 新世界就绪：补应用此前因"没有世界上下文"而被跳过的音量与混音状态
    UE_LOG(LogZombieKing, Verbose, TEXT("[音频] 地图加载完成，补应用音量与混音：%s"), *GetNameSafe(LoadedWorld));

    ApplyVolumeSettings();
    ApplyAudioStateMix();
}

// =============================================================================
//  音量
// =============================================================================

void UGameAudioSubsystem::ApplyVolumeSettings()
{
    EnsureAudioAssetsLoaded();

    UGameSettingsSubsystem* SettingsService = GetSettingsService();
    UObject* WorldContext = GetAudioWorldContext();

    // 无世界上下文（启动早期）或缺少必需资产：跳过 —— 世界就绪后由 HandlePostLoadMap 自动补应用
    if (!SettingsService || !SettingsSoundMix || !WorldContext)
    {
        return;
    }

    // -------------------------------------------------------------------------
    // 四个音量分组：主 / 音乐 / 音效 / 语音
    // -------------------------------------------------------------------------
    // 用"设置专用 SoundMix"对四个 SoundClass 施加运行期音量覆盖：
    //  - 不污染资产本身（SoundClass 资产上的 Volume 仍是设计基准值）；
    //  - bApplyToChildren = true：主音量自动作用于其下所有子 SoundClass（子类再乘自己的音量）。
    struct FVolumeChannel
    {
        USoundClass* SoundClass;
        float Volume;
    };

    const FVolumeChannel Channels[] =
    {
        { MasterSoundClass, SettingsService->GetMasterVolume() },
        { MusicSoundClass,  SettingsService->GetMusicVolume()  },
        { SFXSoundClass,    SettingsService->GetSFXVolume()    },
        { VoiceSoundClass,  SettingsService->GetVoiceVolume()  },
    };

    const UGameAudioSettings* AudioSettings = GetDefault<UGameAudioSettings>();
    const float FadeSeconds = AudioSettings ? AudioSettings->VolumeFadeSeconds : 0.2f;

    for (const FVolumeChannel& Channel : Channels)
    {
        // 某个分组没配 SoundClass 时跳过（启动加载阶段已逐项告警，这里不重复刷屏）
        if (!Channel.SoundClass)
        {
            continue;
        }

        UGameplayStatics::SetSoundMixClassOverride(WorldContext, SettingsSoundMix, Channel.SoundClass,
            Channel.Volume, /*Pitch=*/1.0f, FadeSeconds, /*bApplyToChildren=*/true);
    }
}

// =============================================================================
//  混音状态
// =============================================================================

void UGameAudioSubsystem::SetAudioState(EGameAudioState NewState)
{
    if (NewState == AudioState)
    {
        // 状态未变也重试一次应用：上一次可能因"无世界上下文"而没真正生效（例如世界刚就绪）
        ApplyAudioStateMix();
        return;
    }

    AudioState = NewState;
    ApplyAudioStateMix();
}

USoundMix* UGameAudioSubsystem::ResolveStateMix(EGameAudioState State) const
{
    switch (State)
    {
    case EGameAudioState::Menu:
    case EGameAudioState::Boot:
        // 启动期即前端氛围：直接沿用菜单混音（避免"启动时无声压"的突兀感）
        return MenuSoundMix;

    case EGameAudioState::Match:
        return MatchSoundMix;

    case EGameAudioState::Spectate:
        return SpectateSoundMix;

    default:
        return nullptr;
    }
}

void UGameAudioSubsystem::ApplyAudioStateMix()
{
    UObject* WorldContext = GetAudioWorldContext();
    if (!WorldContext)
    {
        // 无世界：等 HandlePostLoadMap 时补应用（状态本身已记录在 AudioState 里）
        return;
    }

    USoundMix* DesiredMix = ResolveStateMix(AudioState);

    // 幂等：已经压着目标混音就什么都不做 —— 本函数会被"状态切换 / 世界就绪 / 启动装配"多处调用
    if (PushedStateMix == DesiredMix)
    {
        return;
    }

    // 先弹出上一次压入的混音，再压入目标的：Push/Pop 必须成对，
    // 否则同一个 Mix 被反复 Push 会在音频设备里堆叠残留（表现为混音越切越怪）
    if (PushedStateMix)
    {
        UGameplayStatics::PopSoundMixModifier(WorldContext, PushedStateMix);
        PushedStateMix = nullptr;
    }

    if (DesiredMix)
    {
        UGameplayStatics::PushSoundMixModifier(WorldContext, DesiredMix);
        PushedStateMix = DesiredMix;
    }
}

// =============================================================================
//  播放
// =============================================================================

void UGameAudioSubsystem::PlayAudioEventAtLocation(FGameplayTag EventTag, FVector Location, AActor* InstigatorActor)
{
    FGameAudioEventEntry Entry;
    if (!ResolveEventEntry(EventTag, Entry))
    {
        return;
    }

    // 优先用触发者的世界作为播放上下文（多世界/客户端视角下更精确）；没有则退回本 GameInstance 的当前世界
    UObject* WorldContext = InstigatorActor ? static_cast<UObject*>(InstigatorActor) : GetAudioWorldContext();
    PlayEventInternal(Entry, WorldContext, nullptr, NAME_None, Location, InstigatorActor);
}

void UGameAudioSubsystem::PlayAudioEventAttached(FGameplayTag EventTag, USceneComponent* AttachToComponent, FName AttachSocketName)
{
    if (!AttachToComponent)
    {
        return;
    }

    FGameAudioEventEntry Entry;
    if (!ResolveEventEntry(EventTag, Entry))
    {
        return;
    }

    // 挂接组件自身即播放上下文（它一定属于某个世界）
    PlayEventInternal(Entry, AttachToComponent, AttachToComponent, AttachSocketName, FVector::ZeroVector, AttachToComponent->GetOwner());
}

void UGameAudioSubsystem::PlayAudioEvent2D(FGameplayTag EventTag)
{
    FGameAudioEventEntry Entry;
    if (!ResolveEventEntry(EventTag, Entry))
    {
        return;
    }

    // 2D 播放（UI 音 / 全局播报）：不受空间衰减影响，但仍需要一个世界上下文来拿音频设备
    PlayEventInternal(Entry, GetAudioWorldContext(), nullptr, NAME_None, FVector::ZeroVector, nullptr);
}

bool UGameAudioSubsystem::PlayEventInternal(const FGameAudioEventEntry& Entry, UObject* WorldContext,
                                           USceneComponent* AttachToComponent, FName AttachSocketName,
                                           const FVector& Location, AActor* InstigatorActor)
{
    if (!WorldContext)
    {
        // 没有世界上下文（启动早期 / 专用服务器无音频设备）：静默跳过，不刷告警（属预期降级）
        UE_LOG(LogZombieKing, Verbose, TEXT("[音频] 跳过 %s：当前没有可用于播放的世界上下文"), *Entry.EventTag.ToString());
        return false;
    }

    // 冷却闸门：同一标签在冷却期内只播一次（防高频事件刷爆音轨）
    if (!CheckCooldown(Entry))
    {
        return false;
    }

    // 声音资产按需加载（软引用；找不到时告警一次并返回）
    USoundBase* Sound = Entry.Sound.LoadSynchronous();
    if (!Sound)
    {
        if (!WarnedMissingEventTags.Contains(Entry.EventTag))
        {
            WarnedMissingEventTags.Add(Entry.EventTag);
            UE_LOG(LogZombieKing, Warning, TEXT("[音频] 事件 %s 的声音资产加载失败：%s"),
                *Entry.EventTag.ToString(), *Entry.Sound.ToString());
        }
        return false;
    }

    // 音高随机：同一音效反复播放（脚步、连发）时消除"机关枪式重复感"，属听感质量的关键项
    const float Pitch = (Entry.PitchVariation > 0.0f)
        ? FMath::FRandRange(1.0f - Entry.PitchVariation, 1.0f + Entry.PitchVariation)
        : 1.0f;

    const float Volume = FMath::Max(Entry.VolumeMultiplier, 0.0f);

    // 衰减覆盖（可选；留空则用声音资产自带设置）
    USoundAttenuation* Attenuation = Entry.AttenuationOverride.LoadSynchronous();

    if (Entry.bPlayAs2D)
    {
        // 2D：UI 音、全局播报（bIsUISound = true，不受 3D 空间化与音量衰减影响）
        UGameplayStatics::PlaySound2D(WorldContext, Sound, Volume, Pitch, /*StartTime=*/0.0f,
            /*ConcurrencySettings=*/nullptr, InstigatorActor, /*bIsUISound=*/true);
    }
    else if (AttachToComponent)
    {
        // 挂点：跟随组件（武器挂点、骨骼挂点），把声音"钉"在发声体上
        UGameplayStatics::SpawnSoundAttached(Sound, AttachToComponent, AttachSocketName, FVector::ZeroVector,
            FRotator::ZeroRotator, EAttachLocation::KeepRelativeOffset, /*bStopWhenAttachedToDestroyed=*/true,
            Volume, Pitch, /*StartTime=*/0.0f, Attenuation, /*ConcurrencySettings=*/nullptr, /*bAutoDestroy=*/true);
    }
    else
    {
        // 世界位置：爆炸点、弹着点等"一次性、不跟随"的发声
        UGameplayStatics::SpawnSoundAtLocation(WorldContext, Sound, Location, FRotator::ZeroRotator,
            Volume, Pitch, /*StartTime=*/0.0f, Attenuation, /*ConcurrencySettings=*/nullptr, /*bAutoDestroy=*/true);
    }

    return true;
}

bool UGameAudioSubsystem::CheckCooldown(const FGameAudioEventEntry& Entry)
{
    if (Entry.CooldownSeconds <= 0.0f)
    {
        return true;
    }

    // 用进程单调时钟而不是世界时间：冷却属于"同一进程内的节流"，与世界时钟无关，也不受换图影响
    const float NowSeconds = static_cast<float>(FPlatformTime::Seconds());

    if (const float* LastPlayTime = LastPlayTimeByTag.Find(Entry.EventTag))
    {
        if ((NowSeconds - *LastPlayTime) < Entry.CooldownSeconds)
        {
            // 冷却期内：静默拒绝（属预期行为，不告警）
            return false;
        }
    }

    // 记录本次播放时刻：冷却判定以"是否真的播放"为准
    LastPlayTimeByTag.Add(Entry.EventTag, NowSeconds);
    return true;
}

// =============================================================================
//  内部工具
// =============================================================================

bool UGameAudioSubsystem::ResolveEventEntry(FGameplayTag EventTag, FGameAudioEventEntry& OutEntry)
{
    EnsureAudioAssetsLoaded();

    if (!EventTable)
    {
        // 未配置事件表：整条播放链路都不可用 —— 只在首次请求时告警一次避免刷屏
        if (!WarnedMissingEventTags.Contains(EventTag))
        {
            WarnedMissingEventTags.Add(EventTag);
            UE_LOG(LogZombieKing, Warning, TEXT("[音频] 未加载到音频事件表，无法播放 %s（请内容侧建 DA_AudioEventTable）"), *EventTag.ToString());
        }
        return false;
    }

    if (!EventTable->FindEntry(EventTag, OutEntry))
    {
        // 表中没有该标签：这是"内容侧漏配"最常见的信号，告警一次即可定位（含具体标签名）
        if (!WarnedMissingEventTags.Contains(EventTag))
        {
            WarnedMissingEventTags.Add(EventTag);
            UE_LOG(LogZombieKing, Warning, TEXT("[音频] 音频事件表中没有标签 %s（该音效不会播放；请内容侧补齐）"), *EventTag.ToString());
        }
        return false;
    }

    return true;
}

void UGameAudioSubsystem::EnsureAudioAssetsLoaded()
{
    // 只尝试一次：避免每次播放都去查磁盘（资产加载失败也已告警过）
    if (bAudioAssetsLoadAttempted)
    {
        return;
    }
    bAudioAssetsLoadAttempted = true;

    const UGameAudioSettings* AudioSettings = GetDefault<UGameAudioSettings>();
    if (!AudioSettings)
    {
        return;
    }

    // -------------------------------------------------------------------------
    // 统一加载助手：软引用 → 同步加载（缺失时告警并返回 null，绝不崩）
    // 关键：加载结果必须存入成员强引用，否则会被 GC 立刻回收（本项目已记录过的坑）。
    // -------------------------------------------------------------------------
    auto LoadSoundClass = [this](const TSoftObjectPtr<USoundClass>& SoftPtr, const TCHAR* Label) -> USoundClass*
    {
        USoundClass* Asset = SoftPtr.LoadSynchronous();
        if (!Asset)
        {
            UE_LOG(LogZombieKing, Warning, TEXT("[音频] 未找到 %s 音量对应的 SoundClass：%s（该音量将无法调整）"), Label, *SoftPtr.ToString());
        }
        return Asset;
    };

    auto LoadSoundMix = [this](const TSoftObjectPtr<USoundMix>& SoftPtr, const TCHAR* Label) -> USoundMix*
    {
        USoundMix* Asset = SoftPtr.LoadSynchronous();
        if (!Asset)
        {
            UE_LOG(LogZombieKing, Warning, TEXT("[音频] 未找到混音 %s：%s（该状态将没有额外混音效果）"), Label, *SoftPtr.ToString());
        }
        return Asset;
    };

    // 四个音量分组的 SoundClass
    MasterSoundClass = LoadSoundClass(AudioSettings->MasterSoundClass, TEXT("主"));
    MusicSoundClass = LoadSoundClass(AudioSettings->MusicSoundClass, TEXT("音乐"));
    SFXSoundClass = LoadSoundClass(AudioSettings->SFXSoundClass, TEXT("音效"));
    VoiceSoundClass = LoadSoundClass(AudioSettings->VoiceSoundClass, TEXT("语音"));

    // 设置专用混音 + 各状态混音
    SettingsSoundMix = LoadSoundMix(AudioSettings->SettingsSoundMix, TEXT("设置音量专用"));
    MenuSoundMix = LoadSoundMix(AudioSettings->MenuSoundMix, TEXT("前端"));
    MatchSoundMix = LoadSoundMix(AudioSettings->MatchSoundMix, TEXT("对局"));
    SpectateSoundMix = LoadSoundMix(AudioSettings->SpectateSoundMix, TEXT("观战"));

    // 音频事件表
    EventTable = AudioSettings->EventTable.LoadSynchronous();
    if (!EventTable)
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[音频] 未找到音频事件表：%s（所有按标签播放的请求都会被跳过）"), *AudioSettings->EventTable.ToString());
    }
}

UObject* UGameAudioSubsystem::GetAudioWorldContext() const
{
    // 说明：UGameInstanceSubsystem 没有 GetWorld()，必须经 GameInstance 取（本项目已记录的引擎事实）。
    // 返回 Non-const 的 UObject* 是为了满足 UGameplayStatics 的 WorldContextObject 形参（它接收 const UObject*，
    // 这里给非 const 是可接受的隐式转换）。
    const UGameInstance* GameInstance = GetGameInstance();
    return GameInstance ? GameInstance->GetWorld() : nullptr;
}

UGameSettingsSubsystem* UGameAudioSubsystem::GetSettingsService() const
{
    // 惰性取用 + 判空：服务之间只保证"同一宿主"，不保证初始化先后
    const UGameInstance* GameInstance = GetGameInstance();
    return GameInstance ? GameInstance->GetSubsystem<UGameSettingsSubsystem>() : nullptr;
}

UGameFlowSubsystem* UGameAudioSubsystem::GetFlowService() const
{
    const UGameInstance* GameInstance = GetGameInstance();
    return GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
}
