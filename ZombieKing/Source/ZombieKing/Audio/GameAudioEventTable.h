#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"   // FGameplayTag（音频事件主键）
#include "GameAudioEventTable.generated.h"

class USoundBase;         // 声音资产（SoundCue / MetaSoundSource 等）
class USoundAttenuation;  // 空间衰减设置

/**
 * 竞技生化网游：单条音频事件（GameplayTag → 声音资产 + 播放参数）
 *
 * 设计要点（大厂做法：玩法只报"事件"，音频资源由音频侧统一管理）：
 *  - **玩法/技能/UI 只报标签**（如 Audio.Weapon.Fire），不知道也不需要知道用哪个声音资产；
 *  - 音频同学在本表里把标签映射到资产与参数，替换资源、调整音量/音高/冷却都不碰玩法代码；
 *  - 一张表全项目共用 → 重复标签、漏配资产都能在资产校验里一次性查出来（见表的 IsDataValid）。
 */
USTRUCT(BlueprintType)
struct FGameAudioEventEntry
{
    GENERATED_BODY()

    /** 音频事件标签（本表主键；约定以 Audio. 开头，分层点分命名，如 Audio.Weapon.Fire） */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Audio")
    FGameplayTag EventTag;

    /**
     * 声音资产（软引用：只在真正播放时才加载；这也让表本身可被 Cook 扫描而不强拉内存）。
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Audio")
    TSoftObjectPtr<USoundBase> Sound;

    /** 音量倍率（1 = 原样；<1 更轻）。注意：这是"该事件相对其 SoundClass 音量"的额外缩放，不是玩家音量设置 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Audio", meta = (ClampMin = "0.0", ClampMax = "10.0"))
    float VolumeMultiplier = 1.0f;

    /**
     * 音高随机幅度（0 = 不随机；0.1 = ±10%）。
     * 用途：同一音效连续播放时（脚步、连续开火）避免"机关枪式重复感"——这是听感质量的关键调参项。
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Audio", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float PitchVariation = 0.0f;

    /**
     * 是否按 2D（无空间衰减）播放。
     * true = UI 音、全局播报；false = 世界内音效（按位置/挂点做空间化）。
     * 说明：2D/3D 由音频侧按"这条音效是否应该体现方位"决定，不需要玩法侧关心。
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Audio")
    bool bPlayAs2D = false;

    /**
     * 冷却（秒）：同一标签在该时间内最多播放一次；0 = 不限制。
     * 用途：防"高频事件刷爆音轨"（例如脚步在急停时被连续触发、UI 按钮被连点）。
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Audio", meta = (ClampMin = "0.0", ClampMax = "5.0"))
    float CooldownSeconds = 0.0f;

    /**
     * 空间衰减覆盖（软引用；留空则用声音资产自带的衰减设置）。
     * 用途：同一份声音资产在不同事件上需要不同听距时（例如"自己的枪声"比"别人的枪声"听得更远）无需复制资产。
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Audio")
    TSoftObjectPtr<USoundAttenuation> AttenuationOverride;
};

/**
 * 竞技生化网游：音频事件表（全项目唯一的声音资源映射表）
 *
 * 为什么是"一张数据资产表"而不是"Lua 文案式表格"（本项目的取舍，理由如下）：
 *  - **声音资产本身是打包内容**：换一条音效无论如何都要重新打包/走资源热更，因此把表放 Lua 换取的
 *    "改表即生效"收益极小；反过来，表放资产能拿到编辑器下拉、类型校验、Cook 扫描与资产生命周期管理；
 *  - 与已有的内容管线一致（`UGameGameData` / `UGameUIRouteTable` / `UGameLocalizationSettings::UITextTable`
 *    都是"资产承载内容、代码只读"）。
 *
 * 分工边界（务必遵守）：
 *  - 本表只回答"**这个标签用什么声音、以什么参数播**"；
 *  - "**什么时候播**"完全属于玩法：由 C++ 派发事件、Lua 规则/表现层决定（见 GameCharacterBase.lua 的开火/换弹/空仓音效）。
 *
 * 内容侧使用：
 *  1) 建资产（约定路径 /Game/Audio/DA_AudioEventTable），逐条填"标签 → 声音资产 + 参数"；
 *  2) 把该资产填进 `UGameAudioSettings::EventTable`（或直接用默认约定路径）；
 *  3) 用资产校验（右键 → Validate）检查重复标签与漏配资产。
 */
UCLASS(BlueprintType)
class ZOMBIEKING_API UGameAudioEventTable : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    /** 全部音频事件（表的主键是 EventTag，不允许重复） */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Audio")
    TArray<FGameAudioEventEntry> Events;

    /**
     * 主资产标识覆写（必须）：
     * `UPrimaryDataAsset` 默认拿"最上层原生类名"当类型，与内容管线登记口径对不上；
     * 这里固定为 `AudioEventTable`（与 GameData / UIRouteTable / WeaponDefinition 同一做法）。
     */
    virtual FPrimaryAssetId GetPrimaryAssetId() const override;

    /** 按标签查表；找到时填充 OutEntry 并返回 true */
    UFUNCTION(BlueprintPure, Category = "Audio")
    bool FindEntry(FGameplayTag EventTag, FGameAudioEventEntry& OutEntry) const;

#if WITH_EDITOR
    /**
     * 资产校验（编辑器右键 Validate / 保存时触发）：
     * 抓"漏填标签、标签重复、漏配声音资产、命名不合约定"这四类最容易导致线上静默无声的错误。
     */
    virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};
