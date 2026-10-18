#include "GameHUDWidget.h"
#include "GamePlayerState.h"                 // 玩家状态（阵营 + ASC 生命属性）
#include "GameGameState.h"                   // 对局状态（阶段 / 阶段剩余时间 / 比分）
#include "GameAttributeSet.h"                // 生命属性访问器（GetHealthAttribute / GetMaxHealthAttribute）
#include "AbilitySystemComponent.h"          // ASC：GetNumericAttribute / GetGameplayAttributeValueChangeDelegate
#include "GameplayEffectTypes.h"             // FOnAttributeChangeData 完整定义
#include "GameFramework/PlayerController.h"  // GetOwningPlayer() 返回的类型与 GetPlayerState<T>()
#include "Kismet/GameplayStatics.h"          // GetGameState(const UObject*)（const 安全的取法）

UGameHUDWidget::UGameHUDWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // 绑定 Lua 表现脚本（脚本根目录 = Content/Script，目录层级用 . 连接）
    LuaScriptPath = TEXT("Lua.Core.UI.GameHUDWidget");
}

FString UGameHUDWidget::GetModuleName_Implementation() const
{
    return LuaScriptPath;
}

// =============================================================================
//  控件生命周期
// =============================================================================

void UGameHUDWidget::NativeConstruct()
{
    Super::NativeConstruct();

    // 首次尝试绑定数据源；若此刻 PlayerState（本机玩家）尚未复制到位，
    // 会由 NativeTick 的幂等刷新兜底重试——这是应对"控件先于网络数据创建"的标准做法。
    RefreshDataSources();
}

void UGameHUDWidget::NativeDestruct()
{
    // 控件被移除/销毁前解绑全部订阅，避免回调打到已失效的控件上
    UnbindPlayerState();
    UnbindGameState();

    Super::NativeDestruct();
}

void UGameHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
    Super::NativeTick(MyGeometry, InDeltaTime);

    // 1) 幂等刷新绑定：内部只做指针比较，实例未变则零成本；变了才重绑并推送当前值。
    RefreshDataSources();

    // 2) 转发给 Lua 做"连续变化"的表现刷新（典型：对局倒计时数字）。
    //    之所以放在 Lua：倒计时格式、提示音、低时间告警等属可热更的表现逻辑。
    OnHUDTick(InDeltaTime);
}

// =============================================================================
//  只读访问
// =============================================================================

AGamePlayerState* UGameHUDWidget::GetGamePlayerState() const
{
    // GetOwningPlayer() 由 CreateWidget 的 Owner 决定，指向本机的 PlayerController，
    // 再由控制器取到本机玩家的 PlayerState（阵营与 GAS 生命属性的宿主）。
    const APlayerController* OwningPC = GetOwningPlayer();
    return OwningPC ? OwningPC->GetPlayerState<AGamePlayerState>() : nullptr;
}

AGameGameState* UGameHUDWidget::GetGameGameState() const
{
    // 用 GameplayStatics 取 GameState：对 const 上下文友好，且内部已处理"世界无 GameState"的情况
    return Cast<AGameGameState>(UGameplayStatics::GetGameState(this));
}

float UGameHUDWidget::GetHealth() const
{
    if (const AGamePlayerState* PS = GetGamePlayerState())
    {
        // 生命属性住在 PlayerState 托管的 ASC 上（Pawn 只是短生命周期 Avatar），
        // 因此即便感染变异换过 Pawn，这里读到的仍是同一个 ASC 的最新值。
        if (const UAbilitySystemComponent* ASC = PS->GetAbilitySystemComponent())
        {
            return ASC->GetNumericAttribute(UGameAttributeSet::GetHealthAttribute());
        }
    }
    return 0.0f;
}

float UGameHUDWidget::GetMaxHealth() const
{
    if (const AGamePlayerState* PS = GetGamePlayerState())
    {
        if (const UAbilitySystemComponent* ASC = PS->GetAbilitySystemComponent())
        {
            return ASC->GetNumericAttribute(UGameAttributeSet::GetMaxHealthAttribute());
        }
    }
    return 0.0f;
}

float UGameHUDWidget::GetHealthPercent() const
{
    const float Max = GetMaxHealth();
    // 上限为 0（属性尚未初始化）时返回 0，避免除零
    return (Max > 0.0f) ? (GetHealth() / Max) : 0.0f;
}

float UGameHUDWidget::GetMatchPhaseRemainingTime() const
{
    const AGameGameState* GS = GetGameGameState();
    return GS ? GS->GetMatchPhaseRemainingTime() : 0.0f;
}

// =============================================================================
//  数据源绑定（机制层）
// =============================================================================

void UGameHUDWidget::RefreshDataSources()
{
    AGamePlayerState* CurrentPS = GetGamePlayerState();
    AGameGameState* CurrentGS = GetGameGameState();

    const bool bPlayerStateChanged = (CurrentPS != BoundPlayerState.Get());
    const bool bGameStateChanged = (CurrentGS != BoundGameState.Get());

    // 先解绑"已变化"的旧绑定（未变化的不动，避免每帧反复摘挂委托）
    if (bPlayerStateChanged)
    {
        UnbindPlayerState();
    }
    if (bGameStateChanged)
    {
        UnbindGameState();
    }

    // 再绑定新实例
    if (bPlayerStateChanged && CurrentPS)
    {
        BindPlayerState(CurrentPS);
    }
    if (bGameStateChanged && CurrentGS)
    {
        BindGameState(CurrentGS);
    }

    // 重绑后主动推送一次"当前值"：属性/委托只在"发生变化"时回调，
    // 若绑定完成时数值已是最终值，不推送就会导致 Lua 显示空白，故此处在绑定时补一次初始同步。
    if (bPlayerStateChanged && CurrentPS)
    {
        OnHUDHealthChanged(GetHealth(), GetMaxHealth(), 0.0f);
        // 初始同步：OldFaction 与 NewFaction 相同，语义为"当前阵营"
        const EGameFaction Faction = CurrentPS->GetPlayerFaction();
        OnHUDFactionChanged(Faction, Faction);
    }
    if (bGameStateChanged && CurrentGS)
    {
        // 初始同步：OldPhase 与 NewPhase 相同，语义为"当前阶段"
        const EGameMatchPhase Phase = CurrentGS->GetMatchPhase();
        OnHUDMatchPhaseChanged(Phase, Phase);
        OnHUDTeamScoresChanged();
    }
}

void UGameHUDWidget::BindPlayerState(AGamePlayerState* InPlayerState)
{
    if (!InPlayerState)
    {
        return;
    }

    // 1) 阵营变化：动态多播委托 → AddDynamic（要求接收方为 UFUNCTION）
    InPlayerState->OnPlayerFactionChanged.AddDynamic(this, &UGameHUDWidget::HandlePlayerFactionChanged);

    // 2) 生命 / 生命上限：GAS 的属性变化委托是"非动态"多播委托 → AddUObject，并保存句柄以便精确解绑
    if (UAbilitySystemComponent* ASC = InPlayerState->GetAbilitySystemComponent())
    {
        HealthChangedDelegateHandle = ASC->GetGameplayAttributeValueChangeDelegate(
            UGameAttributeSet::GetHealthAttribute()).AddUObject(this, &UGameHUDWidget::HandleHealthAttributeChanged);
        MaxHealthChangedDelegateHandle = ASC->GetGameplayAttributeValueChangeDelegate(
            UGameAttributeSet::GetMaxHealthAttribute()).AddUObject(this, &UGameHUDWidget::HandleMaxHealthAttributeChanged);
    }

    BoundPlayerState = InPlayerState;
}

void UGameHUDWidget::UnbindPlayerState()
{
    if (AGamePlayerState* OldPS = BoundPlayerState.Get())
    {
        // 动态委托用 RemoveDynamic 解绑（与 AddDynamic 成对）
        OldPS->OnPlayerFactionChanged.RemoveDynamic(this, &UGameHUDWidget::HandlePlayerFactionChanged);

        // 非动态委托从句柄精确移除
        if (UAbilitySystemComponent* ASC = OldPS->GetAbilitySystemComponent())
        {
            if (HealthChangedDelegateHandle.IsValid())
            {
                ASC->GetGameplayAttributeValueChangeDelegate(UGameAttributeSet::GetHealthAttribute())
                   .Remove(HealthChangedDelegateHandle);
            }
            if (MaxHealthChangedDelegateHandle.IsValid())
            {
                ASC->GetGameplayAttributeValueChangeDelegate(UGameAttributeSet::GetMaxHealthAttribute())
                   .Remove(MaxHealthChangedDelegateHandle);
            }
        }
    }

    HealthChangedDelegateHandle.Reset();
    MaxHealthChangedDelegateHandle.Reset();
    BoundPlayerState.Reset();
}

void UGameHUDWidget::BindGameState(AGameGameState* InGameState)
{
    if (!InGameState)
    {
        return;
    }

    // 阶段变化与比分变化均为动态多播委托 → AddDynamic
    InGameState->OnGameMatchPhaseChanged.AddDynamic(this, &UGameHUDWidget::HandleMatchPhaseChanged);
    InGameState->OnGameTeamScoresChanged.AddDynamic(this, &UGameHUDWidget::HandleTeamScoresChanged);

    BoundGameState = InGameState;
}

void UGameHUDWidget::UnbindGameState()
{
    if (AGameGameState* OldGS = BoundGameState.Get())
    {
        OldGS->OnGameMatchPhaseChanged.RemoveDynamic(this, &UGameHUDWidget::HandleMatchPhaseChanged);
        OldGS->OnGameTeamScoresChanged.RemoveDynamic(this, &UGameHUDWidget::HandleTeamScoresChanged);
    }

    BoundGameState.Reset();
}

// =============================================================================
//  委托回调 → 转发为 Lua 事件
// =============================================================================

void UGameHUDWidget::HandleHealthAttributeChanged(const FOnAttributeChangeData& Data)
{
    // Delta 语义：新值 - 旧值（正为回血、负为受伤）；透传给 Lua 做血条增减动画与受击飘字。
    // 上限在此重新读取而非缓存，保证"上限变化"与"生命变化"两条链路取值一致。
    OnHUDHealthChanged(Data.NewValue, GetMaxHealth(), Data.NewValue - Data.OldValue);
}

void UGameHUDWidget::HandleMaxHealthAttributeChanged(const FOnAttributeChangeData& Data)
{
    // 上限变化时血条比例需要重算；对上限自身而言 Delta 无表现意义，故传 0。
    OnHUDHealthChanged(GetHealth(), Data.NewValue, 0.0f);
}

void UGameHUDWidget::HandlePlayerFactionChanged(EGameFaction NewFaction, EGameFaction OldFaction)
{
    // 阵营变化驱动准星变色、阵营提示、感染演出等（均由 Lua 表现脚本决定）
    OnHUDFactionChanged(NewFaction, OldFaction);
}

void UGameHUDWidget::HandleMatchPhaseChanged(EGameMatchPhase NewPhase, EGameMatchPhase OldPhase)
{
    // 阶段变化驱动阶段横幅、倒计时启停、结算提示等（均由 Lua 表现脚本决定）
    OnHUDMatchPhaseChanged(NewPhase, OldPhase);
}

void UGameHUDWidget::HandleTeamScoresChanged()
{
    // 比分变化驱动比分板刷新；不带参数，Lua 收到后自行调用 GetTeamScore 取值
    OnHUDTeamScoresChanged();
}
