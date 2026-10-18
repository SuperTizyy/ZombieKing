#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"   // UDeveloperSettings（配置暴露到 Project Settings 并落到 ini）
#include "GameLocalizationSettings.generated.h"

class UStringTable;   // UE 文案表资产（UI 文案的载体）

/**
 * 竞技生化网游：单个受支持语言的选项
 *
 * 用途：设置界面的"语言下拉列表"直接用本结构渲染 —— 语言代码（写回引擎）+ 自称（显示给玩家）。
 * 设计要点：**语言名称不做本地化**（行业惯例）：语言列表若跟随当前语言翻译，
 *          玩家一旦切错语言就再也找不到自己的语言；因此这里固定使用"该语言自身"的书写（如 "简体中文" / "English"）。
 */
USTRUCT(BlueprintType)
struct FGameLanguageOption
{
    GENERATED_BODY()

    /** 语言代码（BCP-47 风格，与引擎/ICU 口径一致，如 "zh-Hans" / "en"；大小写不敏感比较） */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Language")
    FString CultureCode;

    /** 该语言的**自称**（用该语言本身书写，如 "简体中文" / "English"） */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Language")
    FString NativeName;
};

/**
 * 竞技生化网游：本地化配置
 * 位置：Project Settings → Game → ZombieKing 本地化（也可直接改 Config/DefaultGame.ini 的
 *       [/Script/ZombieKing.GameLocalizationSettings]）
 *
 * 只放"连接性配置"（支持哪些语言、基准语言是哪个、UI 文案表在哪、对局中是否允许切语言），
 * 不放文案内容本身 —— 文案属于内容资产（UI 文案表）与 Lua 脚本（玩法文案），
 * 二者都能在不重编译 C++ 的前提下调整（UI 文案改表 + 重烤 LocRes；玩法文案改 Lua 即刻生效）。
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "ZombieKing 本地化"))
class ZOMBIEKING_API UGameLocalizationSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UGameLocalizationSettings();

    // -------------------------------------------------------------------------
    // 语言清单
    // -------------------------------------------------------------------------
    /**
     * 受支持语言清单（用户已拍板首发：简体中文 + 英文）。
     * 语义：**这是本项目唯一的"合法语言白名单"** —— 玩家保存的语言码、外部传入的语言码
     *       都必须落在这份清单内，否则一律回退到 NativeCulture（见 UGameLocalizationSubsystem）。
     * 扩展：加语种只改本配置 + 补文案资源，无需改任何 C++ 代码。
     */
    UPROPERTY(Config, EditAnywhere, Category = "Language")
    TArray<FGameLanguageOption> SupportedLanguages;

    /**
     * 基准（Native / 回退）语言代码（用户已拍板："zh-Hans"）。
     * 双重含义：① 文案的**撰写语言**（UI 文案表的源文本、Lua 文案表的基准表都以它书写）；
     *          ② **缺失回退目标** —— 请求的语言不受支持、或某条文案在该语言下缺失时，一律回退到它。
     */
    UPROPERTY(Config, EditAnywhere, Category = "Language")
    FString NativeCulture = TEXT("zh-Hans");

    // -------------------------------------------------------------------------
    // 文案载体
    // -------------------------------------------------------------------------
    /**
     * UI 文案表资产（软引用；内容约定路径 /Game/Localization/ST_GameUI）。
     *
     * 分工（用户已拍板的"双层"方案）：
     *  - **UI 文案**（界面控件的固定文案）走本表：源文本用基准语言撰写、其它语种由 LocRes 提供；
     *    运行时经 UGameLocalizationSubsystem::GetUIText(TextId) 取 FText，可被引擎本地化管线收集/打包；
     *  - **玩法文案**（技能名、提示、结算播报等需要热更的文本）走 Lua 文案表
     *    （Content/Script/Lua/Core/Localization/Localization.lua）—— 改文案即刻生效、无需重打包。
     */
    UPROPERTY(Config, EditAnywhere, Category = "Text")
    TSoftObjectPtr<UStringTable> UITextTable;

    // -------------------------------------------------------------------------
    // 切换策略
    // -------------------------------------------------------------------------
    /**
     * 是否允许在对局中切换语言（默认关闭）。
     * 默认关闭的原因（引擎事实）：文化切换会清空 Slate 字体缓存并重载本地化资源
     * （见 TextLocalizationManager 中关于 font/composite-font cache 的注释），
     * 在竞技对局中触发会造成掉帧与"字形/文案短暂错乱"，故只允许在前端（主菜单/房间）切换。
     * 需要放开时改本配置即可，无需改代码。
     */
    UPROPERTY(Config, EditAnywhere, Category = "Language")
    bool bAllowLanguageSwitchDuringMatch = false;

    /** Project Settings 中的归类：归入 Game，与其它项目配置并列 */
    virtual FName GetCategoryName() const override;
};
