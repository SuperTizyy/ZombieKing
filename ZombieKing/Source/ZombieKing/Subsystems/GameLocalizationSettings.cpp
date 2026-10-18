#include "GameLocalizationSettings.h"

#include "Internationalization/StringTable.h"   // UStringTable（文案表资产类型：软引用默认值需要它）
#include "UObject/SoftObjectPath.h"             // FSoftObjectPath（构造软引用默认值）

UGameLocalizationSettings::UGameLocalizationSettings()
{
    // -------------------------------------------------------------------------
    // 首发语言集合（用户已拍板）：简体中文 + 英文，基准语言为简体中文
    // 说明：NativeName 一律用"该语言自身"书写（语言列表不做本地化，见头文件注释）。
    // -------------------------------------------------------------------------
    {
        FGameLanguageOption Chinese;
        Chinese.CultureCode = TEXT("zh-Hans");
        Chinese.NativeName = TEXT("简体中文");
        SupportedLanguages.Add(MoveTemp(Chinese));

        FGameLanguageOption English;
        English.CultureCode = TEXT("en");
        English.NativeName = TEXT("English");
        SupportedLanguages.Add(MoveTemp(English));
    }

    // 内容约定路径：UI 文案表固定放 /Game/Localization/ST_GameUI（内容侧建好后无需再改配置）
    // 注意：软引用必须写成"包路径.资产名"的完整对象路径，引擎据此定位资产。
    UITextTable = TSoftObjectPtr<UStringTable>(FSoftObjectPath(TEXT("/Game/Localization/ST_GameUI.ST_GameUI")));
}

FName UGameLocalizationSettings::GetCategoryName() const
{
    // 归入 Project Settings 的 "Game" 分类（默认会落到 "Plugins"，不符合本项目的配置组织习惯）
    return FName(TEXT("Game"));
}
