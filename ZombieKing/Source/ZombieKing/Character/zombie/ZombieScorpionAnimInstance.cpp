#include "ZombieScorpionAnimInstance.h"
#include "CrawlerClimbMode.h"

UZombieScorpionAnimInstance::UZombieScorpionAnimInstance()
{
    // 绑定蝎子专属动画脚本（继承通用动画基类脚本，承载本体态特有的热更新调参与表现）。
    // 说明：脚本路径可在动画蓝图 Class Defaults 中覆盖，便于后续拆分更细的变体脚本。
    LuaScriptPath = TEXT("Lua.Core.Animation.ZombieScorpionAnimInstance");
}

void UZombieScorpionAnimInstance::NativeInitializeAnimation()
{
    Super::NativeInitializeAnimation();

    // 爬墙动画槽位自诊断（仅当宿主与 Mover 已就绪时才校验，避免误报）：
    // 蝎子具备爬墙能力，槽位为空属于配置缺失，必须显式暴露
    if (MoverComponent && !ClimbBlendSpace)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] 未配置 ClimbBlendSpace：爬墙移动状态将播放绑定姿态（请在动画蓝图 Class Defaults → Anim|Slots 中赋值）"), *GetName());
    }
}

void UZombieScorpionAnimInstance::RefreshMovementState(float DeltaSeconds)
{
    Super::RefreshMovementState(DeltaSeconds);

    // 爬墙判定：模式名与爬墙模式唯一键比对（与 UCrawlerClimbMode 注册、
    // UCrawlerAttachTransition 切换使用同一套命名，全管线语义一致）
    bIsClimbing = (MovementModeName == CrawlerModeNames::Climbing);
}
