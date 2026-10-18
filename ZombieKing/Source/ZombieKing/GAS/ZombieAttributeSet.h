#pragma once

#include "CoreMinimal.h"
#include "GameAttributeSet.h"           // 公共属性基类（承载生命等通用属性）
#include "ZombieAttributeSet.generated.h" // UHT 生成头文件，必须置于最后

/**
 * 竞技生化网游：生化幽灵属性集
 *
 * 架构定位：
 *  - 继承 UGameAttributeSet 的全部通用属性（Health / MaxHealth），
 *    在其之上承载"只有生化阵营才拥有"的专有属性；
 *  - 与 UHumanAttributeSet 对称，二者共同构成"人类 ↔ 生化"双阵营的属性契约，
 *    支撑变异时按需整体切换属性集而不破坏 GAS 状态。
 *
 * 热更新边界（竞技网游铁律）：
 *  - 本类只定义"不可热更的数据结构"——属性字段与复制声明；
 *  - 具体数值、成长曲线与触发规则一律由 GameplayEffect / 数据资产在 Lua 侧配置。
 *
 * 当前状态：契约骨架。
 *  - 生化幽灵专有属性尚未定稿，故本类暂不声明任何属性字段，仅确立类分层与扩展位置；
 *  - 待玩法数值确认后再补 UPROPERTY + GZ_ATTRIBUTE_ACCESSORS + OnRep 复制链路。
 *
 * TODO（待玩法确认后填入，仅示意候选，不代表最终设计）：
 *  - 爬墙能量 ClimbEnergy / MaxClimbEnergy（驱动 Mover 自定义 Climbing 模式的持续消耗与恢复）；
 *  - 感染强度 InfectionPower（命中人类时施加的感染进度）。
 */
UCLASS()
class ZOMBIEKING_API UZombieAttributeSet : public UGameAttributeSet
{
	GENERATED_BODY()

public:
	/**
	 * 构造函数：保持空实现。
	 * 生化幽灵专有属性的初始数值同样由"初始化 GameplayEffect"在服务器灌入，不在此写死，
	 * 以满足热更新调参要求。
	 */
	UZombieAttributeSet();
};
