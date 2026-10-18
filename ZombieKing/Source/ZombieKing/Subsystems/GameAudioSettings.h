#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"   // UDeveloperSettings（配置暴露到 Project Settings 并落到 ini）
#include "GameAudioSettings.generated.h"

class UGameAudioEventTable;   // 音频事件表（标签 → 声音）
class USoundClass;            // 声音类（音量分组的载体）
class USoundMix;              // 混音（对 SoundClass 施加运行期覆盖/压制的载体）

/**
 * 竞技生化网游：音频配置
 * 位置：Project Settings → Game → ZombieKing 音频（也可直接改 Config/DefaultGame.ini 的
 *       [/Script/ZombieKing.GameAudioSettings]）
 *
 * 只放"连接性配置"（事件表在哪、四个音量挂在哪个 SoundClass、各状态用哪个 SoundMix），
 * 不放任何声音资源与参数 —— 那些在 UGameAudioEventTable 与各 SoundClass/SoundMix 资产里。
 *
 * 音量与混音的分工（本项目的落地方式，理由见 UGameAudioSubsystem 类注释）：
 *  - **四个音量设置**（主/音乐/音效/语音）→ 各对应一个 SoundClass；运行期用"设置专用 SoundMix"
 *    对这四个 SoundClass 施加音量覆盖（引擎的 SetSoundMixClassOverride），这样既能被玩家实时调整，
 *    又不污染资产本身的音量；
 *  - **动态混音状态**（菜单 / 对局 / 观战）→ 各对应一个 SoundMix，用 Push/Pop 切换，
 *    用于"对局中压低音乐""观战时突出脚步"这类整段氛围变化；
 *  - **单条音效的压制关系**（如枪声压住音乐）不需要写代码：在 SoundClass 资产的 Duck 关系里配。
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "ZombieKing 音频"))
class ZOMBIEKING_API UGameAudioSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UGameAudioSettings();

    // -------------------------------------------------------------------------
    // 事件表
    // -------------------------------------------------------------------------
    /**
     * 音频事件表（软引用，约定路径 /Game/Audio/DA_AudioEventTable）。
     * 未配置或加载失败时，所有"按标签播放"的请求会被安全拒绝（只告警，不影响玩法）。
     */
    UPROPERTY(Config, EditAnywhere, Category = "Audio")
    TSoftObjectPtr<UGameAudioEventTable> EventTable;

    // -------------------------------------------------------------------------
    // 音量分组（与 UGameSettingsSubsystem 的四个音量一一对应）
    // -------------------------------------------------------------------------
    /** 主音量对应的 SoundClass（通常是根 SoundClass：所有声音都挂在它之下） */
    UPROPERTY(Config, EditAnywhere, Category = "Audio|Volume")
    TSoftObjectPtr<USoundClass> MasterSoundClass;

    /** 音乐音量对应的 SoundClass（背景音乐、菜单音乐） */
    UPROPERTY(Config, EditAnywhere, Category = "Audio|Volume")
    TSoftObjectPtr<USoundClass> MusicSoundClass;

    /** 音效音量对应的 SoundClass（枪声、脚步、命中） */
    UPROPERTY(Config, EditAnywhere, Category = "Audio|Volume")
    TSoftObjectPtr<USoundClass> SFXSoundClass;

    /** 语音音量对应的 SoundClass（队友语音、播报） */
    UPROPERTY(Config, EditAnywhere, Category = "Audio|Volume")
    TSoftObjectPtr<USoundClass> VoiceSoundClass;

    /**
     * "设置音量"专用 SoundMix：四个音量的运行期覆盖都挂在这一个 Mix 上。
     * 为什么单独一个 Mix 而不是复用动态混音状态：设置音量必须在**任何状态下都持续生效**，
     * 而状态 Mix 会被 Push/Pop 切换；两者分开才能避免"切菜单时玩家的音量设置被顶掉"。
     */
    UPROPERTY(Config, EditAnywhere, Category = "Audio|Volume")
    TSoftObjectPtr<USoundMix> SettingsSoundMix;

    /** 音量覆盖的淡入时长（秒）：避免拉动音量滑条时出现"咔"的突变 */
    UPROPERTY(Config, EditAnywhere, Category = "Audio|Volume", meta = (ClampMin = "0.0", ClampMax = "3.0"))
    float VolumeFadeSeconds = 0.2f;

    // -------------------------------------------------------------------------
    // 动态混音状态
    // -------------------------------------------------------------------------
    /** 菜单/前端状态混音（主菜单、房间等待等） */
    UPROPERTY(Config, EditAnywhere, Category = "Audio|State")
    TSoftObjectPtr<USoundMix> MenuSoundMix;

    /** 对局状态混音（战斗中：通常压低音乐、突出枪声与脚步） */
    UPROPERTY(Config, EditAnywhere, Category = "Audio|State")
    TSoftObjectPtr<USoundMix> MatchSoundMix;

    /** 观战状态混音（观战相机下：通常弱化第一人称相关音） */
    UPROPERTY(Config, EditAnywhere, Category = "Audio|State")
    TSoftObjectPtr<USoundMix> SpectateSoundMix;

    /** Project Settings 中的归类：归入 Game，与其它项目配置并列 */
    virtual FName GetCategoryName() const override;
};
