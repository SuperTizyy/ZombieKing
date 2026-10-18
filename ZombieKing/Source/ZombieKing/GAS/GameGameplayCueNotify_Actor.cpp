#include "GameGameplayCueNotify_Actor.h"
#include "ZombieKing.h"   // LogZombieKing

FString AGameGameplayCueNotify_Actor::GetModuleName_Implementation() const
{
    // 优先用资产上配置的脚本；未配置则回落到默认共享脚本（仅打日志，保证通道可用且不报错）。
    // 注意：默认模块名**必须**写成函数内的字符串字面量，不能放匿名命名空间的全局常量 ——
    //       UBT 的 unity build 会把多个 .cpp 合并进同一个翻译单元，不同文件的匿名命名空间同名常量会重定义。
    return LuaScriptPath.IsEmpty()
        ? FString(TEXT("Lua.Core.GAS.GameplayCueNotify_Actor"))
        : LuaScriptPath;
}

void AGameGameplayCueNotify_Actor::HandleGameplayCue(AActor* MyTarget, EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters)
{
    // 先派发 Lua 事件：保证 Lua 在"对象仍有效"时拿到事件
    // （Removed 事件下引擎的 Super 可能回收/销毁自身，故顺序不可颠倒）
    OnGameplayCue(MyTarget, EventType, Parameters);

    // 再走引擎内部状态机：激活、复用（Recycle）、结束回调等逻辑必须由引擎维护，不能跳过
    Super::HandleGameplayCue(MyTarget, EventType, Parameters);
}
