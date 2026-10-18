#include "GameAudioSettings.h"

#include "GameAudioEventTable.h"   // UGameAudioEventTable（软引用默认值需要完整类型）
#include "Sound/SoundClass.h"      // USoundClass
#include "Sound/SoundMix.h"        // USoundMix
#include "UObject/SoftObjectPath.h"// FSoftObjectPath

UGameAudioSettings::UGameAudioSettings()
{
    // -------------------------------------------------------------------------
    // 内容约定路径（内容侧按此路径建资产后即自动生效，无需再改配置）
    // 说明：软引用必须写成"包路径.资产名"的完整对象路径，引擎据此定位资产；
    //       资产不存在时只告警不崩溃（见 UGameAudioSubsystem 的加载与判空逻辑）。
    // -------------------------------------------------------------------------
    EventTable = TSoftObjectPtr<UGameAudioEventTable>(FSoftObjectPath(TEXT("/Game/Audio/DA_AudioEventTable.DA_AudioEventTable")));

    // 四个音量分组（命名约定：SC_ = SoundClass）
    MasterSoundClass = TSoftObjectPtr<USoundClass>(FSoftObjectPath(TEXT("/Game/Audio/SC_Master.SC_Master")));
    MusicSoundClass = TSoftObjectPtr<USoundClass>(FSoftObjectPath(TEXT("/Game/Audio/SC_Music.SC_Music")));
    SFXSoundClass = TSoftObjectPtr<USoundClass>(FSoftObjectPath(TEXT("/Game/Audio/SC_SFX.SC_SFX")));
    VoiceSoundClass = TSoftObjectPtr<USoundClass>(FSoftObjectPath(TEXT("/Game/Audio/SC_Voice.SC_Voice")));

    // 混音（命名约定：SM_ = SoundMix）
    SettingsSoundMix = TSoftObjectPtr<USoundMix>(FSoftObjectPath(TEXT("/Game/Audio/SM_Settings.SM_Settings")));
    MenuSoundMix = TSoftObjectPtr<USoundMix>(FSoftObjectPath(TEXT("/Game/Audio/SM_Menu.SM_Menu")));
    MatchSoundMix = TSoftObjectPtr<USoundMix>(FSoftObjectPath(TEXT("/Game/Audio/SM_Match.SM_Match")));
    SpectateSoundMix = TSoftObjectPtr<USoundMix>(FSoftObjectPath(TEXT("/Game/Audio/SM_Spectate.SM_Spectate")));
}

FName UGameAudioSettings::GetCategoryName() const
{
    // 归入 Project Settings 的 "Game" 分类（默认会落到 "Plugins"，不符合本项目的配置组织习惯）
    return FName(TEXT("Game"));
}
