#include "GameUISettings.h"

UGameUISettings::UGameUISettings()
{
    // 内容约定路径：路由表固定放 /Game/UI/DA_UIRouteTable；内容侧建好后无需再改配置即可生效
    RouteTable = TSoftObjectPtr<UGameUIRouteTable>(FSoftObjectPath(TEXT("/Game/UI/DA_UIRouteTable.DA_UIRouteTable")));
}

FName UGameUISettings::GetCategoryName() const
{
    // 归入 Project Settings 的 "Game" 分类（默认会落到 "Plugins"，不符合本项目的配置组织习惯）
    return FName(TEXT("Game"));
}
