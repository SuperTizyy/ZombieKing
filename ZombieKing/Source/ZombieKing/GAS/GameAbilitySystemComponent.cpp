#include "GameAbilitySystemComponent.h"
#include "GameEffectTags.h"              // GameEffectTags::Data_Damage（SetByCaller 原始伤害）
#include "CombatComponent.h"             // UCombatComponent::DetermineHitZone（爆头判定复用受击方的唯一规则源）
#include "AbilitySystemInterface.h"      // IAbilitySystemInterface（从目标 Actor 取 ASC，解耦具体角色类）
#include "GenericTeamAgentInterface.h"   // IGenericTeamAgentInterface / FGenericTeamId（敌我判定的阵营过滤）
#include "GameplayEffect.h"              // FGameplayEffectSpec / FActiveGameplayEffectHandle（需要完整类型）
#include "GameFramework/Actor.h"         // AActor::HasAuthority

UGameAbilitySystemComponent::UGameAbilitySystemComponent()
{
    // 开启"注册子对象列表"复制模式：
    //  - 这是 AttributeSet（属性集）与其它 ASC 子对象能够网络同步的前提；
    //  - 引擎默认 false，必须在组件构造期打开（该成员为 protected，仅子类可写）；
    //  - 打开后，AddSpawnedAttribute 会把属性集通过 AddReplicatedSubObject 登记，
    //    属性集自身的 Replicated 属性（Health/MaxHealth）才能复制到客户端。
    bReplicateUsingRegisteredSubObjectList = true;
}

bool UGameAbilitySystemComponent::ApplyDamageToTarget(AActor* TargetActor, float DamageAmount,
                                                      TSubclassOf<UGameplayEffect> DamageEffectClass, AActor* DamageSource,
                                                      bool bMeleeDamage)
{
    // 无命中细节的伤害（爆炸 / 范围伤害等）：不携带 FHitResult
    return ApplyDamageInternal(TargetActor, DamageAmount, DamageEffectClass, DamageSource, nullptr, bMeleeDamage);
}

bool UGameAbilitySystemComponent::ApplyDamageToTargetWithHit(AActor* TargetActor, float DamageAmount,
                                                             TSubclassOf<UGameplayEffect> DamageEffectClass, AActor* DamageSource,
                                                             const FHitResult& HitResult, bool bMeleeDamage)
{
    // 带命中细节的伤害：把 FHitResult 写入效果上下文，
    // 使受击方（UCombatComponent）能据此做部位判定（爆头/四肢倍率）与受击硬直。
    return ApplyDamageInternal(TargetActor, DamageAmount, DamageEffectClass, DamageSource, &HitResult, bMeleeDamage);
}

bool UGameAbilitySystemComponent::ApplyDamageInternal(AActor* TargetActor, float DamageAmount,
                                                      TSubclassOf<UGameplayEffect> DamageEffectClass, AActor* DamageSource,
                                                      const FHitResult* HitResult, bool bMeleeDamage)
{
    // -------------------------------------------------------------------------
    // 服务器权威：只有服务器能结算伤害（客户端结算会被 GAS 忽略，且会造成各端数值不一致）
    // -------------------------------------------------------------------------
    // 注意：此处变量刻意命名为 OwningActor —— 不能叫 OwnerActor，
    // 因为 UAbilitySystemComponent 本身就有名为 OwnerActor 的成员，同名会触发 C4458（声明隐藏类成员）。
    const AActor* OwningActor = GetOwner();
    if (!OwningActor || !OwningActor->HasAuthority() || !TargetActor || !DamageEffectClass || DamageAmount <= 0.0f)
    {
        return false;
    }

    // 从目标 Actor 取 ASC：走通用接口（IAbilitySystemInterface），不依赖具体角色类，
    // 因此玩家、AI 生化幽灵、可破坏物只要挂了 ASC 都能被同一套伤害流程处理。
    IAbilitySystemInterface* TargetAbilityInterface = Cast<IAbilitySystemInterface>(TargetActor);
    UAbilitySystemComponent* TargetAbilitySystemComponent =
        TargetAbilityInterface ? TargetAbilityInterface->GetAbilitySystemComponent() : nullptr;
    if (!TargetAbilitySystemComponent)
    {
        return false;
    }

    // 构造效果上下文并写入归因信息：
    // SourceObject = 本 ASC（施加方），Instigator/Causer = 伤害来源 Pawn；
    // HealthComponent 正是从这里取击杀者，用于死亡演出与击杀结算。
    FGameplayEffectContextHandle ContextHandle = MakeEffectContext();
    ContextHandle.AddSourceObject(this);
    if (DamageSource)
    {
        ContextHandle.AddInstigator(DamageSource, DamageSource);
    }

    // 命中细节（可为空）：携带后，受击方（UCombatComponent）能从属性变化回调里取出命中骨骼与来源，
    // 进而做部位判定（爆头/四肢倍率）与受击硬直。这是"服务器结算伤害 + 命中细节随上下文送达"的标准通道。
    if (HitResult)
    {
        ContextHandle.AddHitResult(*HitResult);
    }

    // 生成效果规格：伤害数值通过 SetByCaller 传入（同一份伤害 GE 资产服务所有伤害来源）
    const FGameplayEffectSpecHandle SpecHandle = MakeOutgoingSpec(DamageEffectClass, /*Level=*/1.0f, ContextHandle);
    if (!SpecHandle.IsValid())
    {
        return false;
    }
    SpecHandle.Data->SetSetByCallerMagnitude(GameEffectTags::Data_Damage, DamageAmount);

    // 近战标记：以"动态资产标签"随规格送达受击方（引擎事实：FGameplayEffectSpec::GetAllAssetTags
    // = 动态标签 + GE 定义资产标签，二者受击方都能在属性变化回调里读到）。
    // 刻意不要求内容侧给"近战伤害 GE"单独配 Asset Tags —— 同一份伤害 GE 服务所有来源的既有约定不破。
    if (bMeleeDamage)
    {
        SpecHandle.Data->AddDynamicAssetTag(GameEffectTags::Damage_Source_Melee);
    }

    // 施加到目标：句柄可判断是否真正生效（被免疫 / 无效时不会成功）
    const FActiveGameplayEffectHandle AppliedHandle =
        ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetAbilitySystemComponent);
    const bool bApplied = AppliedHandle.WasSuccessfullyApplied();

    // -------------------------------------------------------------------------
    // 命中确认：把"打中了谁 / 多少伤害 / 是否爆头"发回**攻击者客户端**（纯表现反馈，不参与结算）
    // -------------------------------------------------------------------------
    // 说明：
    //  - Client RPC 由服务器发出，只会到达本 ASC（即攻击者）的属主客户端 —— 这正是"命中标记只属于开枪者"的要求；
    //  - 爆头判定复用**受击方**的 CombatComponent::DetermineHitZone（部位判定只有一份规则实现，
    //    否则攻击者侧再写一套"含 head 即头部"的逻辑，两边迟早漂移）；
    //  - 击杀确认另走一条通道（由受害者的死亡路径发出，见 AGameCharacterBase::HandleHealthDeath）——
    //    本函数不做"是否致死"判断，避免在此处查询目标属性值。
    if (bApplied)
    {
        bool bHeadshot = false;
        if (HitResult)
        {
            if (const UCombatComponent* VictimCombat = TargetActor->FindComponentByClass<UCombatComponent>())
            {
                bHeadshot = (VictimCombat->DetermineHitZone(HitResult->BoneName) == EHitZone::Head);
            }
        }

        ClientNotifyHitConfirmed(TargetActor, DamageAmount, bHeadshot);
    }

    return bApplied;
}

// -------------------------------------------------------------------------
// 命中 / 击杀确认（客户端实现）
// -------------------------------------------------------------------------
void UGameAbilitySystemComponent::ClientNotifyHitConfirmed_Implementation(AActor* HitTarget, float DamageAmount, bool bHeadshot)
{
    // 客户端收到服务器确认：广播给表现层（HUD 命中标记 / 命中音效 / 伤害数字）
    OnHitConfirmed.Broadcast(HitTarget, DamageAmount, bHeadshot);
}

void UGameAbilitySystemComponent::ClientNotifyKillConfirmed_Implementation(AActor* VictimActor)
{
    // 客户端收到服务器确认：广播给表现层（击杀提示 / 击杀音效）
    OnKillConfirmed.Broadcast(VictimActor);
}

// -------------------------------------------------------------------------
// 敌我判定
// -------------------------------------------------------------------------
bool UGameAbilitySystemComponent::IsHostileTarget(AActor* SourceActor, AActor* TargetActor)
{
    // 1) 排除空目标与自伤
    if (!TargetActor || TargetActor == SourceActor)
    {
        return false;
    }

    // 2) 目标必须能被 GAS 结算：没有 ASC 的目标（场景道具、纯装饰体）不参与伤害结算
    if (!Cast<IAbilitySystemInterface>(TargetActor))
    {
        return false;
    }

    // 3) 阵营过滤：双方都提供通用阵营 ID 时，"同阵营且非 NoTeam" 判为友军不可伤害。
    //    用 GetId() 比较而非直接比较结构体，避免不同引擎版本 FGenericTeamId 运算符差异。
    const IGenericTeamAgentInterface* SourceTeamAgent = Cast<IGenericTeamAgentInterface>(SourceActor);
    const IGenericTeamAgentInterface* TargetTeamAgent = Cast<IGenericTeamAgentInterface>(TargetActor);
    if (SourceTeamAgent && TargetTeamAgent)
    {
        const uint8 SourceTeamId = SourceTeamAgent->GetGenericTeamId().GetId();
        const uint8 TargetTeamId = TargetTeamAgent->GetGenericTeamId().GetId();
        if (SourceTeamId != FGenericTeamId::NoTeam.GetId() && SourceTeamId == TargetTeamId)
        {
            return false;
        }
    }

    return true;
}

bool UGameAbilitySystemComponent::TryActivateAbilitiesByInputTag(FGameplayTag InputTag)
{
    // 无效标签直接拒绝：避免把空标签当成"匹配所有技能"误触发
    if (!InputTag.IsValid())
    {
        return false;
    }

    // 组装为标签容器后交给引擎内建的"按标签激活"能力：
    //  - bAllowRemoteActivation = true：允许客户端发起激活请求，GAS 会按技能的 NetExecutionPolicy
    //    决定本地预测还是转发服务器 —— 这是竞技网游"客户端即时反馈 + 服务器裁决"的标准路径。
    FGameplayTagContainer InputTagContainer;
    InputTagContainer.AddTag(InputTag);

    return TryActivateAbilitiesByTag(InputTagContainer, /*bAllowRemoteActivation=*/true);
}
