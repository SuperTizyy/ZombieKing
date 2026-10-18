#include "GameGameInstance.h"
#include "GameAudioSubsystem.h"          // 音频服务（启动期装配：订阅音量设置 + 前端流程 + 首次应用混音）
#include "GameLocalizationSubsystem.h"   // 本地化服务（启动期装配：订阅语言变更 + 应用玩家保存的语言）
#include "ZombieKing.h"                  // LogZombieKing（生命周期日志）

UGameGameInstance::UGameGameInstance()
{
    // 绑定框架层 Lua 脚本（可由蓝图子类覆盖）：
    // GameInstance 级逻辑（全局配置加载、存档冲刷）写在这里，改完即生效、可热更。
    LuaScriptPath = TEXT("Lua.Core.GameFramework.GameInstance");
}

FString UGameGameInstance::GetModuleName_Implementation() const
{
    // 返回 Lua 模块路径；UnLua 运行时会据此把本对象与对应 Lua 脚本绑定
    return LuaScriptPath;
}

void UGameGameInstance::Init()
{
    // 必须先调用 Super：引擎会在其中创建并初始化所有 GameInstanceSubsystem（即"服务层"）
    Super::Init();

    // 服务层就绪后再打日志：确认进入可用状态，便于启动流程排障
    UE_LOG(LogZombieKing, Log, TEXT("GameInstance 初始化完成（服务层已就绪）"));

    // -------------------------------------------------------------------------
    // 跨服务装配（放在这里而不是各服务自己的 OnServiceInitialize，原因见下）：
    // GameInstance 层服务的创建/初始化顺序由引擎决定，**不保证"设置服务先于本地化服务"**；
    // 因此凡是"服务之间需要互相绑定"的动作，一律在本函数里显式做 —— 此时所有服务都已创建完成，
    // 顺序依赖被彻底消除。这也是本项目对"服务间装配"的统一约定。
    // -------------------------------------------------------------------------
    UGameLocalizationSubsystem* LocalizationService = GetSubsystem<UGameLocalizationSubsystem>();
    if (LocalizationService)
    {
        // ① 先把 C++ 侧"语言已切换"事件接到本实例（再转发给 Lua，见 HandleLanguageChanged）；
        //    必须在应用语言之前接好，否则启动期那次切换会漏掉广播。
        LocalizationService->OnLanguageChanged.AddDynamic(this, &UGameGameInstance::HandleLanguageChanged);

        // ② 订阅设置服务的语言变更 + 应用玩家上次保存的语言（读存档 → 校验/回退 → 落地引擎 culture）
        LocalizationService->BindSettingsAndApplyAtStartup();
    }

    // 音频服务装配：订阅"音量设置"变更 + 前端流程阶段（驱动默认混音状态），并首次应用音量与混音。
    // 说明：本服务的音量覆盖与混音都需要"有世界的上下文"，启动早期可能还没有地图 ——
    //       服务内部会自动跳过并注册"地图加载完成"回调，在世界就绪后补应用（见 GameAudioSubsystem）。
    if (UGameAudioSubsystem* AudioService = GetSubsystem<UGameAudioSubsystem>())
    {
        AudioService->BindSettingsAndApplyAtStartup();
    }

    // 再通知 Lua：此时 Lua 才能在 OnInit 中安全取用各服务（顺序不可颠倒）
    OnGameInstanceInit();

    // 把"当前生效语言"推给 Lua，供 Lua 文案层完成首次装载。
    // 说明：上面 BindSettingsAndApplyAtStartup 内部的广播发生在 Lua 尚未初始化时（会被漏掉），
    //       故此处显式补一次；若语言未发生变化，这次推送就是 Lua 首次装载的唯一来源。
    if (LocalizationService)
    {
        OnLanguageChanged(LocalizationService->GetCurrentCultureCode());
    }
}

void UGameGameInstance::HandleLanguageChanged(FString NewCultureCode)
{
    // 只做桥接：C++（本地化服务）→ 本实例 → Lua（同名函数接管）。
    // 规则与表现（重载文案、刷新界面）一律写在 Lua 侧，C++ 不越界。
    OnLanguageChanged(NewCultureCode);
}

void UGameGameInstance::Shutdown()
{
    // 先派发关闭事件：让 Lua 在服务与世界被销毁之前完成落盘、上报等收尾动作
    OnGameInstanceShutdown();

    // 收尾日志
    UE_LOG(LogZombieKing, Log, TEXT("GameInstance 关闭中"));

    // 最后调用 Super：由引擎销毁子系统并释放全局资源
    Super::Shutdown();
}
