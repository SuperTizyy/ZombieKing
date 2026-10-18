#pragma once

#include "CoreMinimal.h"
#include "GameSpectatorTypes.generated.h"

/**
 * 观战相机模式（机制性枚举放 C++）
 *
 * 分层说明（与项目"热更新铁律"一致）：
 *  - 本枚举属于**机制**：它决定"相机如何取景"这条不可热更的契约骨架，必须由 C++ 声明；
 *  - 而每种模式的可调参数（第三人称臂长、碰撞探测半径、飞行速度、混入时长、默认模式）
 *    一律放在 UGameSpectatorComponent / AGameSpectatorPawn 的 EditDefaultsOnly 属性里 ——
 *    内容侧改数值即可生效，不需要重新编译 C++。
 *
 * 之所以单独成头文件（而不是塞进某个类头）：观战 Pawn（AGameSpectatorPawn）与观战组件
 * （UGameSpectatorComponent）都要用它，独立轻量头可避免两者互相 include 形成循环依赖。
 * 本约定与项目既有的 Character/GameWeaponTypes.h 一致。
 */
UENUM(BlueprintType)
enum class EGameSpectatorCameraMode : uint8
{
	/**
	 * 自由视角：相机位于观察者自己驾驶的飞行 Pawn 上，可自由飞行环视。
	 * 按本项目规则**允许穿墙**（观察者 Pawn 关闭碰撞），便于观战/导播看清全局。
	 */
	FreeCam UMETA(DisplayName = "FreeCam (自由视角)"),

	/**
	 * 跟随·第一人称：直接取被观战者的"眼睛位置 + 瞄准朝向"，
	 * 等价于"透过被观战者的眼睛看"，与 CF 生化模式死亡后跟随队友的第一人称视角惯例一致。
	 */
	FollowFirstPerson UMETA(DisplayName = "FollowFirstPerson (跟随·第一人称)"),

	/**
	 * 跟随·第三人称：相机挂在被观战者身后一段距离，由观察者的视角输入绕目标环绕；
	 * 带防穿墙球形扫掠（撞墙自动拉近），因此第三人称**不会穿墙**。
	 */
	FollowThirdPerson UMETA(DisplayName = "FollowThirdPerson (跟随·第三人称)"),

	/** 枚举哨兵：用于容量/越界校验，绝不作为真实模式使用 */
	MAX UMETA(Hidden)
};
