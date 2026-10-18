#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"   // UPrimaryDataAsset（可被 AssetManager 当"主资产"扫描/管理）
#include "GameUITypes.h"        // EGameUILayer（每条路由的目标层级）
#include "GameUIRouteTable.generated.h"

class UGameUserWidgetBase;      // 前端界面基类（路由指向的控件蓝图都派生自它）

/**
 * 一条界面路由：路由名 → 界面类 + 目标层级。
 * 用"软类引用"而非硬引用：避免一份路由表把所有界面蓝图一次性拖进内存，按需加载即可。
 */
USTRUCT(BlueprintType)
struct FGameUIRouteEntry
{
    GENERATED_BODY()

    /** 路由名（全表唯一；建议用稳定的英文短名，如 MainMenu / Settings / Room） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "UI")
    FName Route;

    /** 该路由对应的界面类（派生自 UGameUserWidgetBase 的控件蓝图） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "UI")
    TSoftClassPtr<UGameUserWidgetBase> WidgetClass;

    /** 目标层级（决定 ZOrder 区间与是否接收输入） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "UI")
    EGameUILayer Layer = EGameUILayer::Window;
};

/**
 * 竞技生化网游：界面路由表（数据资产）
 *
 * 为什么把"路由 → 界面类"做成数据资产：
 *  - 新增/调整界面只改内容，不需要重编译 C++（契合热更新纪律）；
 *  - 路由与层级集中一处，UI 管理层只按名字推入界面，不认识具体界面类，职责干净；
 *  - 配合 AssetManager 的主资产扫描（DefaultGame.ini 的 `UIRouteTable` 类型）可保证它必定被 Cook。
 *
 * 内容侧：在 `/Game/UI` 下建一份派生本类的数据资产（如 `DA_UIRouteTable`），逐条填写路由即可。
 * 之后在 `UGameUISettings::RouteTable`（Project Settings → Game → ZombieKing 界面）把它指过来。
 */
UCLASS(BlueprintType)
class ZOMBIEKING_API UGameUIRouteTable : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    /**
     * 覆写主资产 ID：类型固定为 UIRouteTable，名称沿用资产自身名字。
     * 原因同 GameGameData：`UPrimaryDataAsset` 默认拿"最上层原生类名"当类型（GameUIRouteTable），
     * 与 DefaultGame.ini 里登记的类型名对不上，按类型查不到、Cook 规则也不生效。
     */
    virtual FPrimaryAssetId GetPrimaryAssetId() const override;

    /** 全部路由条目（内容侧填写） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "UI")
    TArray<FGameUIRouteEntry> Routes;

    /**
     * 按路由名查条目。
     * @param Route     目标路由名
     * @param OutEntry  命中时写出条目内容
     * @return 是否找到
     */
    UFUNCTION(BlueprintPure, Category = "UI")
    bool FindRoute(FName Route, FGameUIRouteEntry& OutEntry) const;
};
