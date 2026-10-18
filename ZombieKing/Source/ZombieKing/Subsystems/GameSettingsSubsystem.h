#pragma once

#include "CoreMinimal.h"
#include "GameServiceSubsystem.h"   // UGameServiceSubsystem（GameInstance 层服务统一基类）
#include "GameSettingsSubsystem.generated.h"

class UGameGameSettings;            // 本项目自有设置（音量/语言等）的数据对象
class UEnhancedInputUserSettings;   // 引擎按键设置对象（复用引擎，不重复实现）

/** 设置分类：用于"哪一类设置发生了变化"的广播（订阅方按需响应，避免全量重刷） */
UENUM(BlueprintType)
enum class EGameSettingsCategory : uint8
{
    /** 画面（分辨率 / 画质）：由引擎 UGameUserSettings 承载 */
    Graphics UMETA(DisplayName = "画面"),

    /** 音频（各类音量）：由本项目自有设置承载，实际应用交给音频层/Lua */
    Audio    UMETA(DisplayName = "音频"),

    /** 输入（按键重映射）：由引擎 UEnhancedInputUserSettings 承载 */
    Input    UMETA(DisplayName = "输入"),

    /** 语言 */
    Language UMETA(DisplayName = "语言"),

    /** 其它 / 全部设置已应用 */
    Other    UMETA(DisplayName = "其它")
};

/** 设置变更广播（供设置界面刷新、音频层重新应用、遥测等订阅） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameSettingsChanged, EGameSettingsCategory, Category);

/**
 * 竞技生化网游：设置服务（GameInstance 层服务）
 *
 * 设计边界（用户已拍板："复用引擎 + 自建只补缺"）：
 *  - **画面（分辨率/画质）** 由引擎 `UGameUserSettings` 负责 —— 它是这套东西的权威实现，本项目不重复造；
 *  - **输入（按键重映射）** 由引擎 `UEnhancedInputUserSettings` 负责 —— UE5.3+ 自带，天然支持手柄/多平台；
 *  - 本服务只承载**引擎不提供的设置**（音量分类、语言等，见 UGameGameSettings），并为三者提供
 *    **统一入口 + 统一变更事件 + 统一"应用并落盘"** —— 这就是"收口"的价值：
 *    界面只跟本服务打交道，不需要认识引擎那三个分散的设置类。
 *
 * 职责边界（"机制在 C++、数值与规则在 Lua"）：
 *  - 本服务只做**存取 + 变更广播 + 落盘**；
 *  - "音量怎么应用到混音、语言怎么切换"属**表现/规则**，由音频层 / 本地化层 / Lua 订阅变更事件后执行
 *    —— 本服务刻意不直接调音频接口，避免把音频实现细节耦合进设置服务。
 *
 * 生命周期：随 GameInstance 存活；启动时加载设置，关闭时若"脏"则自动落盘。
 */
UCLASS()
class ZOMBIEKING_API UGameSettingsSubsystem : public UGameServiceSubsystem
{
    GENERATED_BODY()

public:
    /** 服务短名（日志前缀 / 按名定位用） */
    virtual FName GetServiceName() const override;

    // -------------------------------------------------------------------------
    // 自有设置对象
    // -------------------------------------------------------------------------
    /** 取本项目自有设置对象（承担音量/语言等；启动时已加载，不会为空） */
    UFUNCTION(BlueprintPure, Category = "Settings")
    UGameGameSettings* GetGameSettings() const { return GameSettings; }

    // -------------------------------------------------------------------------
    // 音量（0..1）
    // -------------------------------------------------------------------------
    UFUNCTION(BlueprintPure, Category = "Settings|Audio")
    float GetMasterVolume() const;

    UFUNCTION(BlueprintCallable, Category = "Settings|Audio")
    void SetMasterVolume(float InVolume);

    UFUNCTION(BlueprintPure, Category = "Settings|Audio")
    float GetMusicVolume() const;

    UFUNCTION(BlueprintCallable, Category = "Settings|Audio")
    void SetMusicVolume(float InVolume);

    UFUNCTION(BlueprintPure, Category = "Settings|Audio")
    float GetSFXVolume() const;

    UFUNCTION(BlueprintCallable, Category = "Settings|Audio")
    void SetSFXVolume(float InVolume);

    UFUNCTION(BlueprintPure, Category = "Settings|Audio")
    float GetVoiceVolume() const;

    UFUNCTION(BlueprintCallable, Category = "Settings|Audio")
    void SetVoiceVolume(float InVolume);

    // -------------------------------------------------------------------------
    // 语言
    // -------------------------------------------------------------------------
    /** 当前语言代码（如 "zh-Hans" / "en"） */
    UFUNCTION(BlueprintPure, Category = "Settings|Language")
    FString GetLanguageCode() const;

    /** 设置语言代码（具体切换动作由本地化层订阅变更事件后执行） */
    UFUNCTION(BlueprintCallable, Category = "Settings|Language")
    void SetLanguageCode(const FString& InLanguageCode);

    // -------------------------------------------------------------------------
    // 引擎设置入口（复用引擎）
    // -------------------------------------------------------------------------
    /**
     * 取引擎的按键设置对象（配合设置界面的"按键重映射"使用）。
     * 说明：它是"每本地玩家"的对象，专用服务器 / 启动早期可能取不到（返回 null，调用方判空）。
     */
    UFUNCTION(BlueprintPure, Category = "Settings|Input")
    UEnhancedInputUserSettings* GetInputUserSettings() const;

    // -------------------------------------------------------------------------
    // 应用与落盘
    // -------------------------------------------------------------------------
    /**
     * 统一收口：应用并保存全部设置。
     * 内部依次：① 引擎画面设置 Apply+Save；② 引擎按键设置 Apply+Save；③ 自有设置落盘；
     *          ④ 广播一次"其它"分类（订阅方如音频层据此重新应用音量）。
     * 界面点"应用"时调它即可，不需要知道背后有三个设置对象。
     */
    UFUNCTION(BlueprintCallable, Category = "Settings")
    void ApplyAndSaveSettings();

    /** 只落盘自有设置（音量/语言等）；帧内频繁改动可先不落盘，退出时自动保存 */
    UFUNCTION(BlueprintCallable, Category = "Settings")
    bool SaveGameSettings();

    /** 从存档加载自有设置（不存在则用默认值；启动时自动调用） */
    UFUNCTION(BlueprintCallable, Category = "Settings")
    bool LoadGameSettings();

    // -------------------------------------------------------------------------
    // 事件（BlueprintAssignable：设置界面 / 音频层 / 本地化层 / 遥测订阅）
    // -------------------------------------------------------------------------
    /** 设置变更事件（携带发生变化的分类） */
    UPROPERTY(BlueprintAssignable, Category = "Settings")
    FOnGameSettingsChanged OnSettingsChanged;

protected:
    /** 启动：加载自有设置（引擎设置由引擎自行在启动时读取 ini，这里无需干预） */
    virtual void OnServiceInitialize() override;

    /** 关闭：若自有设置"脏"，自动落盘（避免玩家刚改完设置就退出导致丢失） */
    virtual void OnServiceDeinitialize() override;

private:
    /** 确保自有设置对象存在（不存在则按默认值新建） */
    void EnsureGameSettings();

    /** 标记"自有设置已变更"：置脏 + 广播对应分类（所有 setter 共用的收尾动作） */
    void MarkDirtyAndBroadcast(EGameSettingsCategory Category);

private:
    /** 自有设置对象（UPROPERTY 持有 = 强引用 = 不被 GC） */
    UPROPERTY(Transient)
    TObjectPtr<UGameGameSettings> GameSettings;

    /** 脏标记：为真时退出前会落盘（避免"改了就写盘"造成的频繁 IO） */
    bool bSettingsDirty = false;
};
