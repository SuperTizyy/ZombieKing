#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"   // UPrimaryDataAsset（可被 AssetManager 当"主资产"扫描/管理的数据资产基类）
#include "GameGameData.generated.h"

/**
 * 竞技生化网游：必备资源清单（GameData 主数据资产）
 *
 * 定位（网游大厂的内容管线惯例）：
 *  - 每个项目有一份（或少量几份）"必备资源"数据资产：列出**启动就要常驻内存**的内容
 *    （全局 GameplayEffect、默认角色定义、默认输入配置、全局音效/材质等）；
 *  - 由 UGameAssetManager 在 `StartInitialLoading` 阶段统一加载，从而：
 *      ① 首批战斗/首次进场景不会因异步加载而卡顿或丢表现；
 *      ② 这些资源不会因为"暂时没被引用"而在 Cook 时被裁掉（配合主资产的 AlwaysCook 规则）；
 *  - **只放真正必须常驻的资源**：随关卡/随场景加载的资产不应放这里，否则会把常驻内存撑大。
 *
 * 内容侧建立方式：在 `/Game/GameData` 下新建派生本类的**数据资产**（如 `DA_GameData`），
 * 往 `EssentialAssets` 里填软引用即可 —— 清单本身是内容，改它不需要重编译 C++。
 *
 * 主资产类型：覆写 `GetPrimaryAssetId` 把类型固定为 `GameData`，
 * 以便 DefaultGame.ini 中登记的主资产扫描条目（`PrimaryAssetType="GameData"`）能稳定对上
 * （引擎默认会拿"原生类名"当类型，即 GameGameData，与配置不一致）。
 */
UCLASS(BlueprintType)
class ZOMBIEKING_API UGameGameData : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    /**
     * 覆写主资产 ID：类型固定为 GameData，名称沿用资产自身名字。
     * 之所以要覆写：`UPrimaryDataAsset` 默认取"最上层原生类名"作为类型（这里是 GameGameData），
     * 那样会与 DefaultGame.ini 里我们登记的类型名对不上，导致按类型查不到、Cook 规则也不生效。
     */
    virtual FPrimaryAssetId GetPrimaryAssetId() const override;

    /**
     * 启动即加载（常驻内存）的必备资源清单。
     * 说明：用"软引用"而非硬引用——清单资产本身可以被 Cook/加载，但不会因此把所有资源都硬拖进内存；
     *       真正加载动作由 UGameAssetManager 在启动时统一执行。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Essential")
    TArray<TSoftObjectPtr<UObject>> EssentialAssets;
};
