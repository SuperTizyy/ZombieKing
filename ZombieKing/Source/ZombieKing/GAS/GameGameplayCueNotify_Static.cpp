#include "GameGameplayCueNotify_Static.h"
#include "ZombieKing.h"   // LogZombieKing

FString UGameGameplayCueNotify_Static::GetModuleName_Implementation() const
{
    // 优先用资产上配置的脚本；未配置则回落到默认共享脚本（仅打日志，保证通道可用且不报错）。
    // 注意：默认模块名**必须**写成函数内的字符串字面量，不能放匿名命名空间的全局常量 ——
    //       UBT 的 unity build 会把多个 .cpp 合并进同一个翻译单元，不同文件的匿名命名空间同名常量会重定义。
    return LuaScriptPath.IsEmpty()
        ? FString(TEXT("Lua.Core.GAS.GameplayCueNotify_Static"))
        : LuaScriptPath;
}

void UGameGameplayCueNotify_Static::HandleGameplayCue(AActor* MyTarget, EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters)
{
    // 先派发 Lua 事件：保证 Lua 拿到的永远是"对象仍有效、状态未被引擎改动"的原始事件
    OnGameplayCue(MyTarget, EventType, Parameters);

    // 再走引擎内部分发（静态 Cue 的分发仅做事件转发，不会销毁自身，因此这里调用 Super 是安全的）
    Super::HandleGameplayCue(MyTarget, EventType, Parameters);
}
