#include "GameUIRouteTable.h"

FPrimaryAssetId UGameUIRouteTable::GetPrimaryAssetId() const
{
    // 类型固定为 UIRouteTable（与 DefaultGame.ini 的 +PrimaryAssetTypesToScan 条目一致），名称沿用自身名字
    return FPrimaryAssetId(FPrimaryAssetType(TEXT("UIRouteTable")), GetFName());
}

bool UGameUIRouteTable::FindRoute(FName Route, FGameUIRouteEntry& OutEntry) const
{
    // 路由表是小规模数据（几十条），线性查找即可，无需建索引；
    // 重复路由名不应存在：这里返回首个命中项，重复问题在内容校验（编辑器）阶段暴露。
    for (const FGameUIRouteEntry& Entry : Routes)
    {
        if (Entry.Route == Route)
        {
            OutEntry = Entry;
            return true;
        }
    }
    return false;
}
