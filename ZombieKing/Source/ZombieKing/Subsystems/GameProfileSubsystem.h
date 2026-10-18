#pragma once

#include "CoreMinimal.h"
#include "GameServiceSubsystem.h"   // UGameServiceSubsystem（GameInstance 层服务统一基类）
#include "GameProfileSubsystem.generated.h"

class UGameSaveGame;                // 玩家档案存档对象

/**
 * 竞技生化网游：玩家档案服务（GameInstance 层服务）
 *
 * 职责（只做"机制"）：
 *  - **读**：启动时从本地存档槽加载档案；没有存档则按默认值新建（并记录首次进入时间）；
 *  - **写**：落盘到本地存档槽；提供脏标记，退出时自动保存（避免"改了就写盘"的频繁 IO）；
 *  - **持有**：档案对象常驻内存（UPROPERTY 强引用），供其它系统随时读取。
 *
 * 职责边界：
 *  - 本服务**不定义档案里有什么**——字段在 `UGameSaveGame` 里（结构变更才需要动 C++）；
 *  - 成长、解锁、战绩等玩法数据一律**不在这里设计**：等玩法定稿后再往 UGameSaveGame 加字段
 *    （存档一旦发出去就要兼容，提前乱加字段是最典型的技术债）。
 *
 * 与设置服务的分工：设置是"每台机器"的（见 UGameSettingsSubsystem），档案是"每个玩家"的；
 * 二者用不同存档槽，互不影响（重置设置不会清掉档案，反之亦然）。
 */
UCLASS()
class ZOMBIEKING_API UGameProfileSubsystem : public UGameServiceSubsystem
{
    GENERATED_BODY()

public:
    /** 服务短名（日志前缀 / 按名定位用） */
    virtual FName GetServiceName() const override;

    // -------------------------------------------------------------------------
    // 档案访问
    // -------------------------------------------------------------------------
    /** 取玩家档案对象（启动时已加载/新建，不会为空） */
    UFUNCTION(BlueprintPure, Category = "Profile")
    UGameSaveGame* GetProfile() const { return Profile; }

    // -------------------------------------------------------------------------
    // 档案写入
    // -------------------------------------------------------------------------
    /** 设置玩家显示名并标脏（真正落盘走 SaveProfile 或退出时自动保存） */
    UFUNCTION(BlueprintCallable, Category = "Profile")
    void SetPlayerDisplayName(const FString& InDisplayName);

    /** 标脏：任何修改档案字段的调用方（Lua / 界面）改完后都应调一次 */
    UFUNCTION(BlueprintCallable, Category = "Profile")
    void MarkProfileDirty() { bProfileDirty = true; }

    // -------------------------------------------------------------------------
    // 读 / 写存档
    // -------------------------------------------------------------------------
    /** 落盘档案（成功后会清脏标记；失败则保持脏，退出时会再试） */
    UFUNCTION(BlueprintCallable, Category = "Profile")
    bool SaveProfile();

    /**
     * 从存档槽加载档案。
     * @return 是否成功从存档读到档案（false = 新玩家无存档 / 存档损坏，此时保留当前档案对象）
     */
    UFUNCTION(BlueprintCallable, Category = "Profile")
    bool LoadProfile();

protected:
    /** 启动：建好档案对象 → 尝试加载 → 刷新"最近游玩时间" */
    virtual void OnServiceInitialize() override;

    /** 关闭：若档案"脏"则自动落盘（玩家刚改完就退出也不丢） */
    virtual void OnServiceDeinitialize() override;

private:
    /** 确保档案对象存在（不存在则按默认值新建并填充首次进入时间） */
    void EnsureProfile();

private:
    /** 玩家档案对象（UPROPERTY 持有 = 强引用 = 不被 GC） */
    UPROPERTY(Transient)
    TObjectPtr<UGameSaveGame> Profile;

    /** 脏标记：为真时退出前会落盘 */
    bool bProfileDirty = false;
};
