#pragma once

#include "CoreMinimal.h"
#include "GenericTeamAgentInterface.h"    // FGenericTeamId（通用阵营 ID）
#include "GameFaction.generated.h"

/**
 * 阵营枚举
 *
 * 说明：抽成独立轻量头（不含数据资产依赖），供 PlayerState / GameMode / 角色定义等多处共用。
 * 人类与生化幽灵分属不同阵营，驱动敌我判定（准星变色、友伤过滤）、属性集选择与初始效果。
 */
UENUM(BlueprintType)
enum class EGameFaction : uint8
{
    /** 未指定（尚未确定阵营时的默认值） */
    None UMETA(DisplayName = "None (未指定)"),

    /** 人类幸存者：双足、不可爬墙 */
    Human UMETA(DisplayName = "Human (人类幸存者)"),

    /** 生化幽灵：形态多样（双足/多足、部分可爬墙） */
    Zombie UMETA(DisplayName = "Zombie (生化幽灵)"),

    /** 枚举哨兵：用于数组容量与越界校验，绝不作为真实阵营使用 */
    MAX UMETA(Hidden)
};

/**
 * 阵营 → 通用阵营 ID 的映射约定（项目约定：人类 = 0，生化幽灵 = 1，其余视为无阵营）。
 * 之所以集中在此：IGenericTeamAgentInterface 需要稳定的数值 ID，
 * 分散在各处手写数字极易出现敌我判定不一致。
 */
inline FGenericTeamId GameFactionToTeamId(EGameFaction Faction)
{
    switch (Faction)
    {
    case EGameFaction::Human:  return FGenericTeamId(0);
    case EGameFaction::Zombie: return FGenericTeamId(1);
    default:                   return FGenericTeamId::NoTeam;
    }
}

/** GameFactionToTeamId 的逆映射：把通用阵营 ID 还原为阵营（用于兼容通过 SetGenericTeamId 写入的调用方） */
inline EGameFaction TeamIdToGameFaction(FGenericTeamId TeamId)
{
    switch (TeamId.GetId())
    {
    case 0:  return EGameFaction::Human;
    case 1:  return EGameFaction::Zombie;
    default: return EGameFaction::None;
    }
}
