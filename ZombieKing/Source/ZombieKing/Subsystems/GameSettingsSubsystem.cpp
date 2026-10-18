#include "GameSettingsSubsystem.h"
#include "GameGameSettings.h"                       // UGameGameSettings（自有设置数据）
#include "GameFramework/GameUserSettings.h"         // UGameUserSettings（引擎画面设置：应用 + 落盘）
#include "EnhancedInputSubsystems.h"                // UEnhancedInputLocalPlayerSubsystem（按键设置所在子系统）
#include "UserSettings/EnhancedInputUserSettings.h" // UEnhancedInputUserSettings（引擎按键设置对象）
#include "Engine/LocalPlayer.h"                     // ULocalPlayer::GetSubsystem
#include "Engine/GameInstance.h"                    // GetFirstLocalPlayerController
#include "GameFramework/PlayerController.h"         // APlayerController::GetLocalPlayer
#include "Kismet/GameplayStatics.h"                 // CreateSaveGameObject / SaveGameToSlot / LoadGameFromSlot
#include "ZombieKing.h"                             // LogZombieKing

namespace
{
    /** 自有设置的存档槽名（与玩家档案槽刻意分开：设置是每台机器的、档案是每个玩家的） */
    const TCHAR* const GameSettingsSaveSlot = TEXT("ZombieKingSettings");

    /** 本地用户下标（单机/单用户项目固定 0；将来支持多账号同机时按账号区分） */
    constexpr int32 GameSettingsUserIndex = 0;
}

FName UGameSettingsSubsystem::GetServiceName() const
{
    // 稳定短名：日志前缀与按名定位均依赖它，一经发布不得随意变更
    return FName(TEXT("Settings"));
}

void UGameSettingsSubsystem::OnServiceInitialize()
{
    // 先按默认值建好对象，再尝试用存档中的玩家设置覆盖它：
    // 这样"没有存档（新玩家）"与"存档损坏"两种情况都能安全回落到默认值，不会出现空指针。
    EnsureGameSettings();
    LoadGameSettings();
}

void UGameSettingsSubsystem::OnServiceDeinitialize()
{
    // 只有"脏"才落盘：避免设置没改也写一次档（无谓 IO，也减少存档被意外覆盖的机会）
    if (bSettingsDirty)
    {
        SaveGameSettings();
    }
}

void UGameSettingsSubsystem::EnsureGameSettings()
{
    if (GameSettings)
    {
        return;
    }

    // 创建默认设置对象（CreateSaveGameObject 会按类默认值初始化各字段）
    GameSettings = Cast<UGameGameSettings>(UGameplayStatics::CreateSaveGameObject(UGameGameSettings::StaticClass()));
}

bool UGameSettingsSubsystem::LoadGameSettings()
{
    EnsureGameSettings();

    // 没有存档属正常情况（新玩家）：保留默认值并返回 false，不做告警级别升级
    if (!UGameplayStatics::DoesSaveGameExist(GameSettingsSaveSlot, GameSettingsUserIndex))
    {
        UE_LOG(LogZombieKing, Log, TEXT("[设置] 未找到设置存档，使用默认设置"));
        return false;
    }

    UGameGameSettings* LoadedSettings = Cast<UGameGameSettings>(UGameplayStatics::LoadGameFromSlot(GameSettingsSaveSlot, GameSettingsUserIndex));
    if (!LoadedSettings)
    {
        // 读到了槽位但类型不符（存档损坏/版本错乱）：降级为默认值并告警，绝不因此崩溃
        UE_LOG(LogZombieKing, Warning, TEXT("[设置] 设置存档损坏或类型不符，已回退默认设置"));
        return false;
    }

    GameSettings = LoadedSettings;
    bSettingsDirty = false;
    return true;
}

bool UGameSettingsSubsystem::SaveGameSettings()
{
    EnsureGameSettings();
    if (!GameSettings)
    {
        UE_LOG(LogZombieKing, Error, TEXT("[设置] 保存失败：设置对象不存在"));
        return false;
    }

    const bool bSaved = UGameplayStatics::SaveGameToSlot(GameSettings, GameSettingsSaveSlot, GameSettingsUserIndex);

    // 保存失败则保持"脏"：退出时会再尝试保存一次，避免玩家改动直接丢失
    bSettingsDirty = !bSaved;

    if (!bSaved)
    {
        UE_LOG(LogZombieKing, Error, TEXT("[设置] 保存失败（槽位 %s）"), GameSettingsSaveSlot);
    }
    return bSaved;
}

// -------------------------------------------------------------------------
// 音量
// -------------------------------------------------------------------------
float UGameSettingsSubsystem::GetMasterVolume() const
{
    // 对象缺失时返回"全音量"这一安全默认值，避免调用方拿到 0 而误以为静音
    return GameSettings ? GameSettings->MasterVolume : 1.0f;
}

void UGameSettingsSubsystem::SetMasterVolume(float InVolume)
{
    if (!GameSettings)
    {
        return;
    }
    GameSettings->MasterVolume = FMath::Clamp(InVolume, 0.0f, 1.0f);
    MarkDirtyAndBroadcast(EGameSettingsCategory::Audio);
}

float UGameSettingsSubsystem::GetMusicVolume() const
{
    return GameSettings ? GameSettings->MusicVolume : 1.0f;
}

void UGameSettingsSubsystem::SetMusicVolume(float InVolume)
{
    if (!GameSettings)
    {
        return;
    }
    GameSettings->MusicVolume = FMath::Clamp(InVolume, 0.0f, 1.0f);
    MarkDirtyAndBroadcast(EGameSettingsCategory::Audio);
}

float UGameSettingsSubsystem::GetSFXVolume() const
{
    return GameSettings ? GameSettings->SFXVolume : 1.0f;
}

void UGameSettingsSubsystem::SetSFXVolume(float InVolume)
{
    if (!GameSettings)
    {
        return;
    }
    GameSettings->SFXVolume = FMath::Clamp(InVolume, 0.0f, 1.0f);
    MarkDirtyAndBroadcast(EGameSettingsCategory::Audio);
}

float UGameSettingsSubsystem::GetVoiceVolume() const
{
    return GameSettings ? GameSettings->VoiceVolume : 1.0f;
}

void UGameSettingsSubsystem::SetVoiceVolume(float InVolume)
{
    if (!GameSettings)
    {
        return;
    }
    GameSettings->VoiceVolume = FMath::Clamp(InVolume, 0.0f, 1.0f);
    MarkDirtyAndBroadcast(EGameSettingsCategory::Audio);
}

// -------------------------------------------------------------------------
// 语言
// -------------------------------------------------------------------------
FString UGameSettingsSubsystem::GetLanguageCode() const
{
    return GameSettings ? GameSettings->LanguageCode : FString();
}

void UGameSettingsSubsystem::SetLanguageCode(const FString& InLanguageCode)
{
    // 空语言码没有意义：拒绝并告警，避免把设置写成非法值
    if (InLanguageCode.IsEmpty())
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[设置] 语言码为空，已忽略"));
        return;
    }

    if (!GameSettings)
    {
        return;
    }
    GameSettings->LanguageCode = InLanguageCode;
    MarkDirtyAndBroadcast(EGameSettingsCategory::Language);
}

// -------------------------------------------------------------------------
// 引擎按键设置入口
// -------------------------------------------------------------------------
UEnhancedInputUserSettings* UGameSettingsSubsystem::GetInputUserSettings() const
{
    // 按键设置是"每本地玩家"的：先拿本机控制器，再经它取 LocalPlayer，最后取 EnhancedInput 子系统
    const UGameInstance* GameInstance = GetGameInstance();
    if (!GameInstance)
    {
        return nullptr;
    }

    APlayerController* LocalPlayerController = GameInstance->GetFirstLocalPlayerController();
    ULocalPlayer* LocalPlayer = LocalPlayerController ? LocalPlayerController->GetLocalPlayer() : nullptr;
    if (!LocalPlayer)
    {
        return nullptr;
    }

    UEnhancedInputLocalPlayerSubsystem* InputSubsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
    return InputSubsystem ? InputSubsystem->GetUserSettings() : nullptr;
}

// -------------------------------------------------------------------------
// 应用与落盘
// -------------------------------------------------------------------------
void UGameSettingsSubsystem::ApplyAndSaveSettings()
{
    // ① 引擎画面设置（分辨率/画质）：ApplySettings(false) 表示以"玩家当前选择"为准，
    //    不因命令行参数（如 QA 常用的 -windowed）而回退 —— 这是玩家点"应用"时的预期行为。
    if (UGameUserSettings* UserSettings = UGameUserSettings::GetGameUserSettings())
    {
        UserSettings->ApplySettings(/*bCheckForCommandLineOverrides=*/false);
        UserSettings->SaveSettings();
    }

    // ② 引擎按键设置（重映射）：应用并落盘（其本身就是 USaveGame）
    if (UEnhancedInputUserSettings* InputUserSettings = GetInputUserSettings())
    {
        InputUserSettings->ApplySettings();
        InputUserSettings->SaveSettings();
    }

    // ③ 本项目自有设置落盘
    SaveGameSettings();

    // ④ 广播一次"其它"分类：告诉订阅方（音频层 / 本地化层等）"设置已整体应用，请重新应用一遍"
    UE_LOG(LogZombieKing, Log, TEXT("[设置] 已应用并保存全部设置"));
    OnSettingsChanged.Broadcast(EGameSettingsCategory::Other);
}

void UGameSettingsSubsystem::MarkDirtyAndBroadcast(EGameSettingsCategory Category)
{
    // 统一收尾动作：置脏（退出时会落盘）+ 广播分类（订阅方按需响应）
    bSettingsDirty = true;
    OnSettingsChanged.Broadcast(Category);
}
