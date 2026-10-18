#include "GameAssetManager.h"
#include "GameGameData.h"     // UGameGameData（必备资源清单）
#include "ZombieKing.h"       // LogZombieKing（内容管线日志）

namespace
{
    /** 必备资源清单的主资产类型名（与 DefaultGame.ini 的 +PrimaryAssetTypesToScan 条目必须一致） */
    const TCHAR* const GameDataPrimaryAssetType = TEXT("GameData");

    /** 内容侧约定：必备资源清单放在此目录下（与 DefaultGame.ini 的 Directories 一致） */
    const TCHAR* const GameDataContentDir = TEXT("/Game/GameData");
}

UGameAssetManager& UGameAssetManager::Get()
{
    // 引擎按 DefaultEngine.ini 的 AssetManagerClassName 实例化本类；
    // 这里做一次安全校验：漏配时拿到的是引擎默认 UAssetManager，必须立刻暴露（否则后续行为会莫名其妙）。
    UGameAssetManager* TypedSingleton = Cast<UGameAssetManager>(UAssetManager::GetIfInitialized());
    checkf(TypedSingleton != nullptr,
        TEXT("AssetManager 类型不符：请检查 Config/DefaultEngine.ini 的 [/Script/Engine.Engine] AssetManagerClassName 是否为 /Script/ZombieKing.GameAssetManager"));

    return *TypedSingleton;
}

void UGameAssetManager::StartInitialLoading()
{
    // 必须先调用 Super：引擎在其中完成"按 AssetManagerSettings 扫描主资产类型"等初始化步骤，
    // 之后 GetPrimaryAssetIdList / GetPrimaryAssetPath 才能查到东西。
    Super::StartInitialLoading();

    // 按"主资产类型"取清单：不依赖资产文件名，内容侧改名不会破坏加载（比按 ID 硬编码名字更稳）
    TArray<FPrimaryAssetId> GameDataIds;
    GetPrimaryAssetIdList(FPrimaryAssetType(GameDataPrimaryAssetType), GameDataIds);

    if (GameDataIds.Num() == 0)
    {
        // 内容尚未建立时属正常情况：只告警一次并继续启动（必备资源缺失必须可降级，不能崩）
        UE_LOG(LogZombieKing, Warning,
            TEXT("[内容管线] 未找到 GameData 主资产（约定放在 %s 下）；必备资源清单为空，启动继续。"),
            GameDataContentDir);
        return;
    }

    // 约定：同类型的 GameData 只应有一个；多于一个说明内容侧重复，取首个并告警避免装配歧义
    if (GameDataIds.Num() > 1)
    {
        UE_LOG(LogZombieKing, Warning,
            TEXT("[内容管线] 发现 %d 个 GameData 主资产，仅使用首个（%s）；请清理重复资产。"),
            GameDataIds.Num(), *GameDataIds[0].ToString());
    }

    // 定位清单资产路径；路径无效说明扫描结果与内容对不上（多为类型/目录配置不一致）
    const FSoftObjectPath GameDataPath = GetPrimaryAssetPath(GameDataIds[0]);
    if (!GameDataPath.IsValid())
    {
        UE_LOG(LogZombieKing, Error, TEXT("[内容管线] GameData 主资产路径无效：%s"),
            *GameDataIds[0].ToString());
        return;
    }

    // 同步加载清单资产本身（它必须在启动期就绪：后续必备资源都靠它列出）。
    // 用 FSoftObjectPath::TryLoad：加载失败（资产被删/路径变更）返回 null，便于进入"降级不崩"分支。
    GameData = Cast<UGameGameData>(GameDataPath.TryLoad());
    if (!GameData)
    {
        UE_LOG(LogZombieKing, Error, TEXT("[内容管线] GameData 加载失败：%s"), *GameDataPath.ToString());
        return;
    }

    // 逐个加载清单内的必备资源，并**强引用持有**（否则加载完立刻被 GC，等于没加载）
    int32 LoadedCount = 0;
    for (const TSoftObjectPtr<UObject>& SoftAsset : GameData->EssentialAssets)
    {
        // 空槽位允许存在（内容侧可能留白），跳过而不告警
        if (SoftAsset.IsNull())
        {
            continue;
        }

        UObject* LoadedAsset = SoftAsset.LoadSynchronous();
        if (LoadedAsset)
        {
            LoadedEssentialAssets.Add(LoadedAsset);
            ++LoadedCount;
        }
        else
        {
            // 单个资源加载失败不影响整体启动，但必须留下线索便于内容侧修正
            UE_LOG(LogZombieKing, Warning, TEXT("[内容管线] 必备资源加载失败：%s"), *SoftAsset.ToString());
        }
    }

    UE_LOG(LogZombieKing, Log, TEXT("[内容管线] 必备资源已加载：清单=%s，成功 %d/%d 项"),
        *GameData->GetName(), LoadedCount, GameData->EssentialAssets.Num());
}
