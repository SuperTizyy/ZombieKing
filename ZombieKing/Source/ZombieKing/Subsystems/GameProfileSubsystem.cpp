#include "GameProfileSubsystem.h"
#include "GameSaveGame.h"                        // UGameSaveGame（玩家档案存档对象）
#include "Kismet/GameplayStatics.h"              // CreateSaveGameObject / SaveGameToSlot / LoadGameFromSlot
#include "Misc/DateTime.h"                       // FDateTime::UtcNow（首次/最近游玩时间）
#include "ZombieKing.h"                          // LogZombieKing

namespace
{
    /** 玩家档案的存档槽名（与设置存档槽刻意分开：档案是每个玩家的、设置是每台机器的） */
    const TCHAR* const GameProfileSaveSlot = TEXT("ZombieKingProfile");

    /** 本地用户下标（单机/单用户项目固定 0；将来支持多账号同机时按账号区分） */
    constexpr int32 GameProfileUserIndex = 0;
}

FName UGameProfileSubsystem::GetServiceName() const
{
    // 稳定短名：日志前缀与按名定位均依赖它，一经发布不得随意变更
    return FName(TEXT("Profile"));
}

void UGameProfileSubsystem::OnServiceInitialize()
{
    // 1) 先建好"新玩家默认档案"：这样"没有存档/存档损坏"都能安全回落到默认值，不会出现空指针；
    // 2) 再尝试用存档覆盖它；
    // 3) 最后刷新"最近游玩时间"（本次启动即一次游玩）。
    EnsureProfile();
    LoadProfile();

    if (Profile)
    {
        Profile->LastPlayTime = FDateTime::UtcNow();
        bProfileDirty = true;
    }
}

void UGameProfileSubsystem::OnServiceDeinitialize()
{
    // 只有"脏"才落盘：避免无改动也写一次档（无谓 IO，也减少存档被意外覆盖的机会）
    if (bProfileDirty)
    {
        SaveProfile();
    }
}

void UGameProfileSubsystem::EnsureProfile()
{
    if (Profile)
    {
        return;
    }

    Profile = Cast<UGameSaveGame>(UGameplayStatics::CreateSaveGameObject(UGameSaveGame::StaticClass()));
    if (!Profile)
    {
        UE_LOG(LogZombieKing, Error, TEXT("[档案] 创建档案对象失败"));
        return;
    }

    // 新玩家：首次进入时间即此刻（读档成功时会被存档里的真实值覆盖回去）
    Profile->FirstPlayTime = FDateTime::UtcNow();
    Profile->LastPlayTime = Profile->FirstPlayTime;
}

bool UGameProfileSubsystem::LoadProfile()
{
    EnsureProfile();

    // 没有存档属正常情况（新玩家）：保留默认档案并返回 false，不做告警级别升级
    if (!UGameplayStatics::DoesSaveGameExist(GameProfileSaveSlot, GameProfileUserIndex))
    {
        UE_LOG(LogZombieKing, Log, TEXT("[档案] 未找到档案存档，按新玩家处理"));
        return false;
    }

    UGameSaveGame* LoadedProfile = Cast<UGameSaveGame>(UGameplayStatics::LoadGameFromSlot(GameProfileSaveSlot, GameProfileUserIndex));
    if (!LoadedProfile)
    {
        // 槽位存在但类型不符（存档损坏/版本错乱）：降级为默认档案并告警，绝不因此崩溃
        UE_LOG(LogZombieKing, Warning, TEXT("[档案] 档案存档损坏或类型不符，已回退默认档案"));
        return false;
    }

    Profile = LoadedProfile;
    bProfileDirty = false;

    // TODO（后续里程碑）：此处按 Profile->SaveVersion 做存档迁移（旧版本读进来后升级字段）。
    //   目前只有版本 1，无迁移逻辑；一旦新增字段必须在此补上，否则老存档会静默丢数据。
    return true;
}

bool UGameProfileSubsystem::SaveProfile()
{
    EnsureProfile();
    if (!Profile)
    {
        UE_LOG(LogZombieKing, Error, TEXT("[档案] 保存失败：档案对象不存在"));
        return false;
    }

    const bool bSaved = UGameplayStatics::SaveGameToSlot(Profile, GameProfileSaveSlot, GameProfileUserIndex);

    // 保存失败则保持"脏"：退出时会再尝试一次，尽量不让玩家改动丢失
    bProfileDirty = !bSaved;

    if (!bSaved)
    {
        UE_LOG(LogZombieKing, Error, TEXT("[档案] 保存失败（槽位 %s）"), GameProfileSaveSlot);
    }
    return bSaved;
}

void UGameProfileSubsystem::SetPlayerDisplayName(const FString& InDisplayName)
{
    if (!Profile)
    {
        return;
    }

    // 显示名允许为空（表示"未设置"），不做额外校验；真正的对外昵称以后端账号为准
    Profile->PlayerDisplayName = InDisplayName;
    MarkProfileDirty();
}
