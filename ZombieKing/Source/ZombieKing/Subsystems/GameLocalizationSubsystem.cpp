#include "GameLocalizationSubsystem.h"

#include "GameFlowSubsystem.h"                          // UGameFlowSubsystem（"是否在对局中"的判定依据）
#include "Engine/GameInstance.h"                        // UGameInstance::GetSubsystem（按类型取其它服务）
#include "Internationalization/Internationalization.h"   // FInternationalization（运行时切换语言 + 区域）
#include "Internationalization/Culture.h"                // FCulture（GetCurrentLanguage()->GetName() 需要完整类型）
#include "Internationalization/StringTable.h"            // UStringTable（UI 文案表资产）
#include "Internationalization/StringTableCore.h"        // FStringTable（查询文本 ID 是否存在）
#include "Internationalization/TextKey.h"                // FTextKey（字符串表键类型）
#include "ZombieKing.h"                                  // LogZombieKing

FName UGameLocalizationSubsystem::GetServiceName() const
{
    // 稳定短名：日志前缀与按名定位均依赖它，一经发布不得随意变更
    return FName(TEXT("Localization"));
}

// -------------------------------------------------------------------------
// 生命周期
// -------------------------------------------------------------------------
void UGameLocalizationSubsystem::OnServiceInitialize()
{
    // ① 加载 UI 文案表：界面文案的唯一来源，必须在首个界面出现之前就位（故用同步加载）
    LoadUITextTable();

    // ② 记录"引擎当前实际 culture"。
    //    引擎在更早的启动阶段已按命令行/系统区域决定了初始 culture；这里先如实记录，
    //    目的是让随后的 ApplyCulture 能正确判定"是否需要切换"（也便于排障时比对差异）。
    CurrentCultureCode = FInternationalization::Get().GetCurrentLanguage()->GetName();
    LogService(FString::Printf(TEXT("引擎初始语言：%s"), *CurrentCultureCode));
}

void UGameLocalizationSubsystem::OnServiceDeinitialize()
{
    // 解绑设置变更委托：服务关闭后回调不得再进来（避免半关闭状态被写入）
    if (bBoundToSettings)
    {
        if (UGameSettingsSubsystem* SettingsService = GetSettingsService())
        {
            SettingsService->OnSettingsChanged.RemoveDynamic(this, &UGameLocalizationSubsystem::HandleSettingsChanged);
        }
        bBoundToSettings = false;
    }

    // 释放文案表强引用与查询期缓存：让服务真正回到"关闭"状态
    UITextTable = nullptr;
    UITextTableId = NAME_None;
    WarnedMissingTextIds.Reset();
}

// -------------------------------------------------------------------------
// 装配（由 UGameGameInstance::Init 调用）
// -------------------------------------------------------------------------
void UGameLocalizationSubsystem::BindSettingsAndApplyAtStartup()
{
    // ① 订阅设置服务的"设置变更"事件：语言属于设置的一部分，变更后由本服务统一落地到引擎 culture。
    //    时机由 UGameGameInstance::Init 保证（所有服务创建完成后），因此不存在服务初始化顺序问题。
    if (!bBoundToSettings)
    {
        if (UGameSettingsSubsystem* SettingsService = GetSettingsService())
        {
            SettingsService->OnSettingsChanged.AddDynamic(this, &UGameLocalizationSubsystem::HandleSettingsChanged);
            bBoundToSettings = true;
        }
        else
        {
            // 设置服务不可用属异常（同一 GameInstance 上必然存在）；只告警不中断：
            // 本进程内语言切换仍可用，只是无法持久化（下次启动会回到引擎默认语言）
            UE_LOG(LogZombieKing, Error, TEXT("[本地化] 设置服务不可用，无法订阅语言变更（语言将无法持久化）"));
        }
    }

    // ② 应用玩家保存的语言（启动最早时机）：读设置里的语言码 → 校验/回退 → 落地引擎 culture。
    //    这样"开机即用玩家上次选择的语言"，无需玩家进设置手动切一次。
    const UGameSettingsSubsystem* SettingsService = GetSettingsService();
    const FString SavedCultureCode = SettingsService ? SettingsService->GetLanguageCode() : FString();
    ApplyCulture(ResolveValidCultureCode(SavedCultureCode));
}

// -------------------------------------------------------------------------
// 语言查询
// -------------------------------------------------------------------------
FString UGameLocalizationSubsystem::GetNativeCultureCode() const
{
    // 配置对象是 CDO（UDeveloperSettings 的取值方式），取不到时返回空串让调用方自行兜底
    const UGameLocalizationSettings* Settings = GetDefault<UGameLocalizationSettings>();
    return Settings ? Settings->NativeCulture : FString();
}

TArray<FGameLanguageOption> UGameLocalizationSubsystem::GetSupportedLanguages() const
{
    const UGameLocalizationSettings* Settings = GetDefault<UGameLocalizationSettings>();
    return Settings ? Settings->SupportedLanguages : TArray<FGameLanguageOption>();
}

bool UGameLocalizationSubsystem::IsCultureSupported(const FString& CultureCode) const
{
    // 空语言码没有意义：直接判定为不支持（避免被后续逻辑当成"有效语言"写进设置）
    if (CultureCode.IsEmpty())
    {
        return false;
    }

    const UGameLocalizationSettings* Settings = GetDefault<UGameLocalizationSettings>();
    if (!Settings)
    {
        return false;
    }

    // 大小写不敏感比较：外部传 "ZH-HANS" 也应被认作 "zh-Hans"（ICU 对大小写不敏感，口径保持一致）
    return Settings->SupportedLanguages.ContainsByPredicate([&CultureCode](const FGameLanguageOption& Option)
    {
        return Option.CultureCode.Equals(CultureCode, ESearchCase::IgnoreCase);
    });
}

bool UGameLocalizationSubsystem::CanChangeLanguageNow() const
{
    const UGameLocalizationSettings* Settings = GetDefault<UGameLocalizationSettings>();

    // 配置放开（内容侧显式允许对局中切换）则无条件允许
    if (Settings && Settings->bAllowLanguageSwitchDuringMatch)
    {
        return true;
    }

    // 默认策略：对局中不允许。
    // 判定依据是前端流程服务的阶段（客户端权威状态：进入对局世界后为 Match），
    // 它不依赖世界内 GameState，因此在"切图/换世界"的过程里判断也不会误判。
    const UGameInstance* GameInstance = GetGameInstance();
    const UGameFlowSubsystem* FlowService = GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
    return !(FlowService && FlowService->IsInMatch());
}

// -------------------------------------------------------------------------
// 语言切换
// -------------------------------------------------------------------------
bool UGameLocalizationSubsystem::SetLanguage(const FString& CultureCode)
{
    // ① 规则闸门：默认不允许在对局中切语言（内容侧可放开，见 CanChangeLanguageNow）
    if (!CanChangeLanguageNow())
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[本地化] 当前不允许切换语言（对局中），已忽略请求：%s"), *CultureCode);
        return false;
    }

    // ② 白名单校验：不在受支持清单内的语言码一律拒绝
    //    （"静默回退"在这里是不合适的：它会让玩家的选择被悄悄改写成别的语言，难以排障）
    if (!IsCultureSupported(CultureCode))
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[本地化] 不支持的语言代码，已忽略请求：%s"), *CultureCode);
        return false;
    }

    // ③ 交给设置服务记录：语言属于玩家设置（存档槽 ZombieKingSettings），
    //    其落盘由设置服务统一负责（界面点"应用"→ ApplyAndSaveSettings，或退出时脏则自动落盘），
    //    因此本服务不自行写档，避免出现"两套持久化路径"。
    //    真正的 culture 落地在 HandleSettingsChanged 中完成 —— 这样"设置值"永远是唯一真源，
    //    任何绕过本接口直接改设置服务的路径也会被同步（收口）。
    UGameSettingsSubsystem* SettingsService = GetSettingsService();
    if (!SettingsService)
    {
        UE_LOG(LogZombieKing, Error, TEXT("[本地化] 设置服务不可用，无法保存语言：%s"), *CultureCode);
        return false;
    }
    SettingsService->SetLanguageCode(CultureCode);

    // ④ 兜底：若尚未完成与设置服务的订阅（极端情况：装配点被跳过），此处直接落地，保证"调用即生效"
    if (!bBoundToSettings)
    {
        ApplyCulture(ResolveValidCultureCode(CultureCode));
    }

    return true;
}

// -------------------------------------------------------------------------
// UI 文案
// -------------------------------------------------------------------------
FText UGameLocalizationSubsystem::GetUIText(FName TextId) const
{
    // 空 ID：无可查，返回空文本（调用方传入空 ID 属编码错误，但不值得为此崩溃）
    if (TextId.IsNone())
    {
        return FText::GetEmpty();
    }

    if (UITextTable && HasUITextId(TextId))
    {
        // 走引擎字符串表取 FText：
        //  - 该 FText 的显示串是"按当前 culture 惰性解析"的，文化切换后引擎会让 Slate 重绘（
        //    FTextLocalizationManager 在文本版本变化时 InvalidateAllWidgets），因此 UI 文案会自动刷新；
        //  - 目标语言缺该条目时，引擎回落到"源文本"（即基准语言撰写的文案）—— 回退策略由此天然满足。
        return FText::FromStringTable(UITextTableId, FTextKey(TextId.ToString()));
    }

    // 回退：显示文本 ID 本身 + 一次性告警（可读、可定位；绝不返回空串造成"神秘空白"）
    WarnMissingTextIdOnce(TextId);
    return FText::AsCultureInvariant(TextId.ToString());
}

bool UGameLocalizationSubsystem::HasUITextId(FName TextId) const
{
    if (!UITextTable || TextId.IsNone())
    {
        return false;
    }

    // 直接查字符串表条目是否存在（比"取出来看是否为空"更准确：键存在但值为空串也算存在）
    return UITextTable->GetStringTable()->FindEntry(FTextKey(TextId.ToString())).IsValid();
}

// -------------------------------------------------------------------------
// 内部实现
// -------------------------------------------------------------------------
void UGameLocalizationSubsystem::HandleSettingsChanged(EGameSettingsCategory Category)
{
    // 只关心语言分类：画面/音频/输入由各自订阅方处理，本服务不越界
    if (Category != EGameSettingsCategory::Language)
    {
        return;
    }

    const UGameSettingsSubsystem* SettingsService = GetSettingsService();
    if (!SettingsService)
    {
        return;
    }

    // 把"设置里记录的语言"同步为引擎当前 culture（校验失败则回退基准语言）
    ApplyCulture(ResolveValidCultureCode(SettingsService->GetLanguageCode()));
}

void UGameLocalizationSubsystem::ApplyCulture(const FString& CultureCode)
{
    // 幂等：与当前生效语言一致则不做任何动作
    // （文化切换会触发字体缓存清理与本地化资源重载，无谓触发代价不小，必须避免）
    if (CultureCode == CurrentCultureCode)
    {
        return;
    }

    // 通过 FInternationalization 切换"语言 + 区域"：
    //  - 只切语言（SetCurrentLanguage）会让数字/日期/度量等区域格式停留在旧文化，故用 LanguageAndLocale；
    //  - 刻意不用 SetCurrentCulture：引擎源码注释称其为 sledgehammer，
    //    它会连"资产组文化（AssetGroupCultures）"一起清掉，本项目保留该能力（可用于分资产组语言）。
    if (!FInternationalization::Get().SetCurrentLanguageAndLocale(CultureCode))
    {
        // 切不动（如 ICU 不认识该 culture）：告警并保持原状态 ——
        // 绝不"先改本服务的状态再回滚"，否则会短暂出现"服务说 A、引擎是 B"的不一致窗口
        UE_LOG(LogZombieKing, Warning, TEXT("[本地化] 引擎拒绝切换到 culture：%s（保持原语言 %s）"), *CultureCode, *CurrentCultureCode);
        return;
    }

    const FString OldCultureCode = CurrentCultureCode;
    CurrentCultureCode = CultureCode;

    // 广播给订阅方：引擎会自动刷新的是"FText 驱动的文案"（见 GetUIText 注释）；
    // 而 Lua 文案表、自行拼接的字符串、语音资源等引擎管不到的部分，需要订阅本事件后自行重载。
    LogService(FString::Printf(TEXT("语言已切换：%s -> %s"), *OldCultureCode, *CultureCode));
    OnLanguageChanged.Broadcast(CultureCode);
}

FString UGameLocalizationSubsystem::ResolveValidCultureCode(const FString& Requested) const
{
    // ① 受支持 → 直接用
    if (IsCultureSupported(Requested))
    {
        return Requested;
    }

    // ② 不支持 → 回退基准（Native）语言：它是文案的撰写语言，也是"什么都不可用"时唯一可读的兜底
    const UGameLocalizationSettings* Settings = GetDefault<UGameLocalizationSettings>();
    if (Settings && !Settings->NativeCulture.IsEmpty())
    {
        if (!Requested.IsEmpty())
        {
            UE_LOG(LogZombieKing, Warning, TEXT("[本地化] 语言 %s 不在受支持清单中，已回退基准语言 %s"), *Requested, *Settings->NativeCulture);
        }
        return Settings->NativeCulture;
    }

    // ③ 连基准语言都没配（配置缺失）：不猜测、不改动 —— 保持引擎当前语言，
    //    避免把玩家语言改成一个本项目根本没准备文案的 culture
    return FInternationalization::Get().GetCurrentLanguage()->GetName();
}

UGameSettingsSubsystem* UGameLocalizationSubsystem::GetSettingsService() const
{
    // 惰性取用 + 判空：GameInstance 层服务之间只保证"同一宿主"，不保证初始化先后，
    // 故一律按需查询，绝不在初始化期缓存指针（缓存会随服务重建/销毁而悬空）
    const UGameInstance* GameInstance = GetGameInstance();
    return GameInstance ? GameInstance->GetSubsystem<UGameSettingsSubsystem>() : nullptr;
}

bool UGameLocalizationSubsystem::LoadUITextTable()
{
    const UGameLocalizationSettings* Settings = GetDefault<UGameLocalizationSettings>();
    if (!Settings || Settings->UITextTable.IsNull())
    {
        // 未配置文案表不是致命错误：界面文案会回退为"显示文本 ID 本身"，便于定位缺失内容
        UE_LOG(LogZombieKing, Warning, TEXT("[本地化] 未配置 UI 文案表（UGameLocalizationSettings::UITextTable），GetUIText 将回退为显示文本 ID"));
        return false;
    }

    // 同步加载：文案表是界面文案的唯一来源，必须在首个界面出现之前就位，不适合异步流式加载
    UStringTable* LoadedTable = Settings->UITextTable.LoadSynchronous();
    if (!LoadedTable)
    {
        UE_LOG(LogZombieKing, Error, TEXT("[本地化] UI 文案表加载失败：%s（界面文案将回退为显示文本 ID）"), *Settings->UITextTable.ToString());
        return false;
    }

    // 强引用保住资产：软引用的 LoadSynchronous() 返回值若不被持有会立即被 GC；
    // 而 FText::FromStringTable 依赖该表在引擎字符串表注册表中常驻，否则文案会解析不出来
    UITextTable = LoadedTable;

    // 记录表 ID（= 资产的对象路径名），FText::FromStringTable 用它定位注册表中的表
    UITextTableId = LoadedTable->GetStringTableId();
    UE_LOG(LogZombieKing, Log, TEXT("[本地化] UI 文案表已加载：%s（表 ID：%s）"), *Settings->UITextTable.ToString(), *UITextTableId.ToString());
    return true;
}

void UGameLocalizationSubsystem::WarnMissingTextIdOnce(FName TextId) const
{
    // 同一 ID 只告警一次：缺失文案常出现在每帧刷新的界面上，逐次告警会直接淹掉日志
    if (WarnedMissingTextIds.Contains(TextId))
    {
        return;
    }
    WarnedMissingTextIds.Add(TextId);

    UE_LOG(LogZombieKing, Warning, TEXT("[本地化] UI 文案 ID 未在文案表中找到：%s（已回退为显示 ID 本身；请内容侧补齐）"), *TextId.ToString());
}
