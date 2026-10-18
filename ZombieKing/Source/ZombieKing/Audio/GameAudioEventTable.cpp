#include "GameAudioEventTable.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"   // FDataValidationContext（AddError/AddWarning）
#endif

FPrimaryAssetId UGameAudioEventTable::GetPrimaryAssetId() const
{
    // 固定类型名 + 资产自身名字，得到稳定的主资产标识（如 AudioEventTable:DA_AudioEventTable）。
    // 必须覆写的原因见头文件；不覆写则类型会被推导为原生类名，与内容管线登记口径不一致。
    return FPrimaryAssetId(FPrimaryAssetType(TEXT("AudioEventTable")), GetFName());
}

bool UGameAudioEventTable::FindEntry(FGameplayTag EventTag, FGameAudioEventEntry& OutEntry) const
{
    // 无效标签直接拒绝：避免空标签命中某条"恰好也没填标签"的错误数据
    if (!EventTag.IsValid())
    {
        return false;
    }

    for (const FGameAudioEventEntry& Entry : Events)
    {
        // 用 MatchesTagExact（精确匹配）而不是 HasTag/MatchesTag（层级匹配）：
        // 音频事件是"具体的一条音效"，父子标签匹配会误命中（例如 Audio.Weapon 命中 Audio.Weapon.Fire）。
        if (Entry.EventTag.MatchesTagExact(EventTag))
        {
            OutEntry = Entry;
            return true;
        }
    }

    return false;
}

#if WITH_EDITOR
EDataValidationResult UGameAudioEventTable::IsDataValid(FDataValidationContext& Context) const
{
    // 先跑父类校验（可能已包含基类规则），再叠加本表规则
    EDataValidationResult Result = Super::IsDataValid(Context);

    // 用于查重：同一标签在一张表里出现两次会让"实际播哪个"取决于遍历顺序，属必须拦下的错误
    TSet<FGameplayTag> SeenTags;

    for (int32 Index = 0; Index < Events.Num(); ++Index)
    {
        const FGameAudioEventEntry& Entry = Events[Index];

        // ① 标签必须有效（漏填标签 = 永远不会被播放到，属静默错误）
        if (!Entry.EventTag.IsValid())
        {
            Context.AddError(FText::FromString(
                FString::Printf(TEXT("音频事件表第 %d 条未填写 EventTag（该条永远不会被播放到）"), Index)));
            Result = EDataValidationResult::Invalid;
            continue;   // 标签无效时后续判重无意义
        }

        // ② 标签不可重复
        if (SeenTags.Contains(Entry.EventTag))
        {
            Context.AddError(FText::FromString(
                FString::Printf(TEXT("音频事件标签重复：%s（实际播哪条取决于遍历顺序，必须唯一）"), *Entry.EventTag.ToString())));
            Result = EDataValidationResult::Invalid;
        }
        else
        {
            SeenTags.Add(Entry.EventTag);
        }

        // ③ 必须配声音资产（漏配 = 播不出来，属静默错误）
        if (Entry.Sound.IsNull())
        {
            Context.AddError(FText::FromString(
                FString::Printf(TEXT("音频事件 %s 未配置声音资产，播放时会静默无声"), *Entry.EventTag.ToString())));
            Result = EDataValidationResult::Invalid;
        }

        // ④ 命名约定：统一以 Audio. 开头（告警而非错误，便于检索与工具化）
        if (!Entry.EventTag.ToString().StartsWith(TEXT("Audio.")))
        {
            Context.AddWarning(FText::FromString(
                FString::Printf(TEXT("音频事件标签 %s 未以 \"Audio.\" 开头，建议统一命名便于检索"),
                    *Entry.EventTag.ToString())));
            if (Result == EDataValidationResult::NotValidated)
            {
                Result = EDataValidationResult::Valid;
            }
        }
    }

    return Result;
}
#endif
