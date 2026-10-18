#include "GameGameData.h"

FPrimaryAssetId UGameGameData::GetPrimaryAssetId() const
{
    // 类型：固定为 GameData（与 DefaultGame.ini 的 +PrimaryAssetTypesToScan 条目一致）
    // 名称：沿用自身名字——数据资产实例即资产名，类默认对象则为类名，两种情况都不会产生歧义；
    //       而"按类型"查找（GetPrimaryAssetIdList）并不依赖名称，故名称无需额外约定。
    return FPrimaryAssetId(FPrimaryAssetType(TEXT("GameData")), GetFName());
}
