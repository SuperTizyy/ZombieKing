#pragma once

#include "CoreMinimal.h"
#include "GameWeaponTypes.generated.h"

/**
 * 竞技生化网游：武器的"开火模式"
 *
 * 为什么用枚举而不是标签：开火模式直接决定**输入与节流的机制行为**（单发只允许点一次、
 * 全自动允许按住连发），属于"不可热更的机制契约"，故放 C++；
 * 而"某把枪的射速/弹匣/伤害"等数值一律放武器定义资产（改资产即生效）。
 *
 * 说明：本项目把"允许什么节奏开火"的裁决放在服务器的武器组件里（射速节流），
 *      客户端只负责"表达开火意愿"（按下/按住），因此即便是全自动武器，
 *      客户端也无法突破服务器射速上限（天然反作弊）。
 */
UENUM(BlueprintType)
enum class EGameWeaponFireMode : uint8
{
    /** 单发：一次输入只开一枪（半自动 / 拉栓） */
    Single UMETA(DisplayName = "单发（半自动）"),

    /** 全自动：按住输入持续开火，实际节奏由服务器按射速（RoundsPerMinute）节流 */
    Auto   UMETA(DisplayName = "全自动")
};

/**
 * 竞技生化网游：武器的"弹道结算方式"
 *
 * Hitscan：开火瞬间从枪口沿准星射线检测，立即结算（步枪/手枪/霰弹枪）；
 * Projectile：生成投射物 Actor 飞行，命中时才结算（RPG/榴弹/投掷物）。
 *
 * 两者的差异不是"手感参数"而是**结算机制**（是否需要生成并复制一个 Actor、
 * 是否需要飞行时间与碰撞预测），故属于 C++ 契约。
 */
UENUM(BlueprintType)
enum class EGameWeaponPayloadKind : uint8
{
    /** 即时射线：服务器一次射线（或多发散布射线）立即结算伤害 */
    Hitscan    UMETA(DisplayName = "即时射线（Hitscan）"),

    /** 投射物：服务器生成投射物 Actor，命中时结算伤害 */
    Projectile UMETA(DisplayName = "投射物（Projectile）")
};
