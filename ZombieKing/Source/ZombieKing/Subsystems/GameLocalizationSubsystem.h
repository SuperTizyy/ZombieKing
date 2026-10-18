#pragma once

#include "CoreMinimal.h"
#include "GameServiceSubsystem.h"        // UGameServiceSubsystem（GameInstance 层服务统一基类）
#include "GameSettingsSubsystem.h"       // EGameSettingsCategory（订阅"语言"分类的设置变更）
#include "GameLocalizationSettings.h"    // FGameLanguageOption（受支持语言清单结构）
#include "GameLocalizationSubsystem.generated.h"

class UStringTable;   // UI 文案表资产（强引用保活，避免加载完即被 GC）

/** 语言已切换广播（携带切换后的 culture 代码；供 Lua 文案层、设置界面、遥测订阅） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameLanguageChanged, FString, NewCultureCode);

/**
 * 竞技生化网游：本地化服务（GameInstance 层服务）
 *
 * 定位：**"语言"这件事的唯一权威出入口**（收口），负责三件机制性工作：
 *  ① **语言落地**：把玩家保存的语言码写成引擎当前 culture（`FInternationalization`），
 *     并保证任何路径改动语言后都只有本服务去落地 —— 避免"设置里是 A、引擎里是 B"的双源不一致；
 *  ② **UI 文案查询**：`GetUIText(TextId)` 把"分层点分文本 ID"解析为 `FText`（来自 UE 文案表），
 *     供界面控件直接绑定（可被引擎本地化管线收集与打包，且文化切换后 Slate 会自动重绘刷新）；
 *  ③ **变更广播**：语言真的变化时广播 `OnLanguageChanged`，让"引擎不会自动刷新"的部分
 *     （Lua 文案表、自行拼接的字符串、语音包等）有机会重新加载。
 *
 * 双层文案分工（用户已拍板）——本服务只管第一层，不重复造 Lua 那一层：
 *  - **UI 文案**：UE 文案表（`UStringTable`）+ `FText`（本服务负责），源文本以基准语言撰写，
 *    其它语种由 LocRes 提供；某条在目标语言缺失时，引擎自动回落"源文本"（即基准语言）——
 *    这正是"回退基准语言"策略的落地方式；
 *  - **玩法文案**：Lua 文案表（`Content/Script/Lua/Core/Localization/Localization.lua`），
 *    改文案即刻生效、无需重打包，满足竞技网游不停机热更要求。
 *
 * 生命周期与装配时机（重要）：
 *  - 随 GameInstance 存活；**游戏启动时的语言应用与订阅不在 OnServiceInitialize 里做**，
 *    因为 GameInstance 层各服务的初始化顺序由引擎决定、不保证"设置服务先于本地化服务"；
 *    改由 `UGameGameInstance::Init()` 在所有服务创建完成后显式调用 `BindSettingsAndApplyAtStartup()`
 *    （显式装配点，彻底消除顺序依赖 —— 这是引擎 `FSubsystemCollectionBase` 之外的本项目约定）。
 *
 * 热更新纪律：本服务不写任何"文案内容"与"支持哪些语言"的判定规则细节 ——
 *            语言清单、基准语言、文案表位置、是否允许对局中切换全部来自配置（
 *            `UGameLocalizationSettings`，改 ini 即可）；文案内容来自资产与 Lua。
 */
UCLASS()
class ZOMBIEKING_API UGameLocalizationSubsystem : public UGameServiceSubsystem
{
    GENERATED_BODY()

public:
    /** 服务短名（日志前缀 / 按名定位用） */
    virtual FName GetServiceName() const override;

    // -------------------------------------------------------------------------
    // 语言查询
    // -------------------------------------------------------------------------
    /** 当前**已生效**的语言代码（引擎 culture 的真实值，非"设置里记录的"值） */
    UFUNCTION(BlueprintPure, Category = "Localization|Language")
    FString GetCurrentCultureCode() const { return CurrentCultureCode; }

    /** 基准（Native / 回退）语言代码（来自配置；文案的撰写语言与缺失回退目标） */
    UFUNCTION(BlueprintPure, Category = "Localization|Language")
    FString GetNativeCultureCode() const;

    /** 受支持语言清单（设置界面的语言下拉列表直接用它的 CultureCode + NativeName） */
    UFUNCTION(BlueprintPure, Category = "Localization|Language")
    TArray<FGameLanguageOption> GetSupportedLanguages() const;

    /** 该语言代码是否在本项目受支持清单内（大小写不敏感） */
    UFUNCTION(BlueprintPure, Category = "Localization|Language")
    bool IsCultureSupported(const FString& CultureCode) const;

    /**
     * 当前这一刻是否允许切换语言。
     * 规则：默认**对局中不允许**（文化切换会清空 Slate 字体缓存并重载本地化资源，
     *       竞技对局中触发会造成掉帧与文案短暂错乱）；内容侧可用配置
     *       `UGameLocalizationSettings::bAllowLanguageSwitchDuringMatch` 放开。
     */
    UFUNCTION(BlueprintPure, Category = "Localization|Language")
    bool CanChangeLanguageNow() const;

    // -------------------------------------------------------------------------
    // 语言切换（BlueprintCallable：供设置界面 / Lua 调用）
    // -------------------------------------------------------------------------
    /**
     * 请求切换语言（幂等：与当前一致时无副作用）。
     * 内部顺序：① 校验"当前是否允许切换" → ② 校验受支持清单 → ③ 写入玩家设置（持久化，重启后仍生效）
     *          → ④ 由设置变更回调落地引擎 culture → ⑤ 广播 OnLanguageChanged。
     * 注意：**持久化交给设置服务**（语言属于玩家设置），因此"设置值"始终是唯一真源；
     *       任何绕过本接口直接改设置服务的路径，也会被本服务的订阅同步落地（收口）。
     * @return 是否被接受（false = 当前不允许 / 不支持该语言 / 设置服务不可用）
     */
    UFUNCTION(BlueprintCallable, Category = "Localization|Language")
    bool SetLanguage(const FString& CultureCode);

    // -------------------------------------------------------------------------
    // UI 文案（UE 本地化表路线；玩法文案请走 Lua 文案表）
    // -------------------------------------------------------------------------
    /**
     * 按"分层点分文本 ID"取 UI 文案（如 UI.Menu.StartGame）。
     * 回退链：目标语言 → （引擎自动）源文本即基准语言 → 都没命中时返回**文本 ID 本身**并一次性告警。
     * 刻意不返回空串：界面出现"神秘空白"比显示一个可定位的 ID 更难排障。
     */
    UFUNCTION(BlueprintPure, Category = "Localization|Text")
    FText GetUIText(FName TextId) const;

    /** 该文本 ID 是否存在于 UI 文案表中（供内容校验工具 / 自动化检查使用） */
    UFUNCTION(BlueprintPure, Category = "Localization|Text")
    bool HasUITextId(FName TextId) const;

    // -------------------------------------------------------------------------
    // 装配入口（由 UGameGameInstance::Init 在所有服务就绪后调用，见类注释）
    // -------------------------------------------------------------------------
    /**
     * 订阅设置服务的语言变更 + 应用玩家已保存的语言（启动最早时机，保证开机即用玩家选择的语言）。
     * 幂等：重复调用不会重复订阅。
     */
    void BindSettingsAndApplyAtStartup();

    // -------------------------------------------------------------------------
    // 事件（BlueprintAssignable：Lua 文案层 / 设置界面 / 遥测订阅）
    // -------------------------------------------------------------------------
    /** 语言已切换（携带切换后的 culture 代码） */
    UPROPERTY(BlueprintAssignable, Category = "Localization")
    FOnGameLanguageChanged OnLanguageChanged;

protected:
    /** 启动：加载 UI 文案表 + 记录引擎当前 culture（不在这里订阅设置服务，原因见类注释） */
    virtual void OnServiceInitialize() override;

    /** 关闭：解绑设置变更委托 + 释放文案表强引用 */
    virtual void OnServiceDeinitialize() override;

private:
    /** 设置变更回调（只处理 Language 分类，其它分类不越界） */
    UFUNCTION()
    void HandleSettingsChanged(EGameSettingsCategory Category);

    /** 把指定语言落到引擎 culture（幂等；失败则保持原状并告警，绝不半途改状态） */
    void ApplyCulture(const FString& CultureCode);

    /** 把"请求的语言"解析为可用的语言代码：不支持 / 为空 → 回退基准语言 → 再兜底保持引擎当前语言 */
    FString ResolveValidCultureCode(const FString& Requested) const;

    /** 惰性取设置服务（服务间只保证"同一宿主"，不保证初始化先后，故一律按需取 + 判空） */
    UGameSettingsSubsystem* GetSettingsService() const;

    /** 加载 UI 文案表（同步；文案表必须在首个界面出现之前就位） */
    bool LoadUITextTable();

    /** 文本 ID 缺失告警（同一 ID 只告警一次，避免每帧刷屏） */
    void WarnMissingTextIdOnce(FName TextId) const;

private:
    /** UI 文案表（强引用保活：加载返回值不持有会立即被 GC，且 FText 依赖该表在注册表中常驻） */
    UPROPERTY(Transient)
    TObjectPtr<UStringTable> UITextTable;

    /** 文案表在引擎字符串表注册表中的 ID（`FText::FromStringTable` 需要它） */
    FName UITextTableId;

    /** 当前已生效的语言代码（引擎 culture 的真实值） */
    FString CurrentCultureCode;

    /** 已告警过的缺失文本 ID（纯 C++ 成员，无需 GC 追踪；用 mutable 以便在 const 查询函数中登记） */
    mutable TSet<FName> WarnedMissingTextIds;

    /** 是否已订阅设置服务的变更事件（保证幂等） */
    bool bBoundToSettings = false;
};
