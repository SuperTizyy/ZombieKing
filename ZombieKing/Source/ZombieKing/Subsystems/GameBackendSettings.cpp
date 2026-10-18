#include "GameBackendSettings.h"

FName UGameBackendSettings::GetCategoryName() const
{
    // 归入 Project Settings 的 "Game" 分类（默认会落到 "Plugins"，不符合本项目"玩法配置在 Game 下"的组织习惯）
    return FName(TEXT("Game"));
}
