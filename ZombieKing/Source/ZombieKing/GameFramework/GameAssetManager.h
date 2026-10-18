#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetManager.h"   // UAssetManager（引擎单例：主资产扫描 / 加载 / 分包 Cook 规则的宿主）
#include "GameAssetManager.generated.h"

class UGameGameData;   // 必备资源清单（启动时由本类加载）

/**
 * 竞技生化网游：项目自有 AssetManager（内容管线入口）
 *
 * 为什么要有自己的 AssetManager 子类：
 *  - 引擎默认的 `UAssetManager` 只是个通用实现；项目必须在它上面挂"自己的内容入口"，
 *    否则"必备资源加载、分包规则、资源命名/类型约定"这些东西无处安放；
 *  - 装配方式：Config/DefaultEngine.ini 的 `[/Script/Engine.Engine]` 下
 *    `AssetManagerClassName=/Script/ZombieKing.GameAssetManager`（引擎据此实例化本类）。
 *
 * 本类职责（严格遵循"机制在 C++、内容与规则在配置/Lua"）：
 *  1) 启动期加载"必备资源清单"（UGameGameData）：把常驻资源一次性同步加载并**强引用持有**，
 *     保证首批战斗/首次进场景不因异步加载而卡顿或丢表现；
 *  2) 对外提供强类型单例访问（Get()）：业务代码用它可以免去到处 Cast；
 *  3) 真正的"主资产类型扫描 / 分包 Chunk / Cook 规则"由**配置**决定（DefaultGame.ini 的
 *     `[/Script/Engine.AssetManagerSettings]`），不在 C++ 里硬编码 —— 内容目录调整无需重编译。
 *
 * 加载失败的处理原则（重要）：必备资源缺失属于"内容没配好"而非"代码错误"，
 * 因此本类一律**降级为告警并继续启动**，绝不因为缺资源而崩溃 —— 竞技网游必须保证可启动。
 */
UCLASS()
class ZOMBIEKING_API UGameAssetManager : public UAssetManager
{
    GENERATED_BODY()

public:
    /**
     * 强类型单例访问。
     * 说明：`UAssetManager::Get()` 返回的是基类引用，业务代码若各自 Cast 会到处散落类型转换；
     *       这里集中一次并做配置校验（若 DefaultEngine.ini 漏配 AssetManagerClassName，
     *       拿到的是引擎默认 UAssetManager，直接以断言暴露配置错误，避免后续诡异行为）。
     */
    static UGameAssetManager& Get();

    /** 引擎启动期回调：先走引擎初始化，再加载必备资源清单 */
    virtual void StartInitialLoading() override;

    /** 取已加载的必备资源清单（未配置内容资产时为 null；BlueprintPure 供 Lua / 蓝图取用） */
    UFUNCTION(BlueprintPure, Category = "ContentPipeline")
    UGameGameData* GetGameData() const { return GameData; }

    /**
     * 取启动期已加载并常驻的必备资源列表（强引用持有）。
     * 注意：这里刻意**不做成 UFUNCTION** —— UHT 明令"UFUNCTION 的参数与返回值不得出现 TObjectPtr"
     *       （见编译错误 "UFunctions cannot take a TObjectPtr as a function parameter or return value"）。
     *       故本访问器只供 C++ 使用；Lua / 蓝图需要读取清单内容时，走 GetGameData() 再读其 EssentialAssets。
     */
    const TArray<TObjectPtr<UObject>>& GetLoadedEssentialAssets() const { return LoadedEssentialAssets; }

private:
    /**
     * 必备资源清单资产（启动加载后常驻）。
     * 用 UPROPERTY 持有 = 强引用 = 保证不被 GC —— 这正是"常驻内存"语义的实现方式。
     */
    UPROPERTY(Transient)
    TObjectPtr<UGameGameData> GameData;

    /**
     * 启动期加载的必备资源本体（强引用持有，防止"加载完因无引用被 GC 掉"）。
     * 说明：仅 `LoadSynchronous()` 而不持有返回值是**常见错误**——资源会立刻被回收，等于没加载。
     */
    UPROPERTY(Transient)
    TArray<TObjectPtr<UObject>> LoadedEssentialAssets;
};
