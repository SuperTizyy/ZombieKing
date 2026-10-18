#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"   // UDeveloperSettings（配置暴露到 Project Settings 并落到 ini）
#include "GameUISettings.generated.h"

class UGameUIRouteTable;

/**
 * 竞技生化网游：界面配置
 * 位置：Project Settings → Game → ZombieKing 界面（也可直接改 Config/DefaultGame.ini 的
 *       [/Script/ZombieKing.GameUISettings]）
 *
 * 只放"连接性配置"（哪份路由表、是否自动切输入模式），不放界面内容本身
 * —— 界面内容属于内容资产与 Lua 脚本，均可在不重编译的前提下调整。
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "ZombieKing 界面"))
class ZOMBIEKING_API UGameUISettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UGameUISettings();

    /**
     * 界面路由表资产（软引用，避免把界面内容硬拖进内存；未配置时 UI 管理层会告警并拒绝推入界面）。
     * 默认指向内容约定路径 /Game/UI/DA_UIRouteTable（内容侧建立该资产后即可直接生效）。
     */
    UPROPERTY(Config, EditAnywhere, Category = "UI")
    TSoftObjectPtr<UGameUIRouteTable> RouteTable;

    /**
     * 是否由 UI 管理层自动切换输入模式与鼠标显隐。
     * 默认开启：有交互界面时切 UIOnly + 显示鼠标，回到对局时切 GameOnly + 隐藏鼠标。
     * 纯服务器/无界面场景可关闭。
     */
    UPROPERTY(Config, EditAnywhere, Category = "UI")
    bool bAutoApplyInputMode = true;

    /** Project Settings 中的归类：归入 Game，与其它项目配置并列 */
    virtual FName GetCategoryName() const override;
};
