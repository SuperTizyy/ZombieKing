#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"   // USaveGame（可被 SaveGameToSlot/LoadGameFromSlot 持久化的存档对象基类）
#include "GameGameSettings.generated.h"

/**
 * 竞技生化网游：本项目**自有设置**的数据对象（存档对象，落盘到本地存档槽）
 *
 * 为什么它只管"引擎没有的设置"（重要设计边界）：
 *  - 画质 / 分辨率 由引擎的 `UGameUserSettings` 负责（它读写 GameUserSettings.ini，是这套东西的权威实现）；
 *  - 按键重映射 由引擎的 `UEnhancedInputUserSettings` 负责（UE5.3+ 自带，本身就是一个 USaveGame，
 *    且天然支持手柄/多平台差异）；
 *  - 本类只承载**引擎不提供**的设置：音量分类、语言、以及将来玩法定稿后新增的偏好项。
 *    重复实现引擎已有的设置只会造成"两套数据打架"，这是刻意避免的。
 *
 * 为什么用 USaveGame 而不是写 ini：
 *  - 与引擎自己的设置对象（UEnhancedInputUserSettings 就是 USaveGame）保持同一套持久化机制，口径统一；
 *  - 设置值是**运行期频繁修改**的数据，走存档槽比"改 ini 再重读"更直接、更少平台差异。
 *
 * 扩展方式：玩法定稿后要加设置项，直接在此加 UPROPERTY 字段（结构变更需改 C++，属协议层）；
 *           而"音量多大、默认语言是哪个"这类**数值**全部由 Lua / 内容侧决定，不需要改这里。
 */
UCLASS(BlueprintType)
class ZOMBIEKING_API UGameGameSettings : public USaveGame
{
    GENERATED_BODY()

public:
    /** 存档版本号：将来字段结构变更时用于做迁移（读到旧版本走升级逻辑），从 1 开始 */
    UPROPERTY(BlueprintReadWrite, Category = "Version")
    int32 SaveVersion = 1;

    // -------------------------------------------------------------------------
    // 音频（音量分类：0..1；具体如何应用到混音由音频层/ Lua 决定，本类只存值）
    // -------------------------------------------------------------------------
    /** 主音量 */
    UPROPERTY(BlueprintReadWrite, Category = "Audio", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MasterVolume = 1.0f;

    /** 音乐音量 */
    UPROPERTY(BlueprintReadWrite, Category = "Audio", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MusicVolume = 0.8f;

    /** 音效音量 */
    UPROPERTY(BlueprintReadWrite, Category = "Audio", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SFXVolume = 1.0f;

    /** 语音音量（队友语音/播报） */
    UPROPERTY(BlueprintReadWrite, Category = "Audio", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float VoiceVolume = 1.0f;

    // -------------------------------------------------------------------------
    // 语言
    // -------------------------------------------------------------------------
    /** 语言代码（BCP-47 风格，如 "zh-Hans" / "en"）；具体切换语言由本地化层执行 */
    UPROPERTY(BlueprintReadWrite, Category = "Language")
    FString LanguageCode = TEXT("zh-Hans");

    // -------------------------------------------------------------------------
    // 玩法偏好
    // -------------------------------------------------------------------------
    // 说明：此处刻意留白 —— 玩法偏好项必须等玩法定稿后再定义（例如是否显示伤害飘字、
    //       准星样式、爬墙特效强度等）。凭空添加字段会与后续设计冲突，届时直接在此追加即可。
};
