#pragma once

#include "CoreMinimal.h"
#include "GameAttributeSet.h"           // 公共属性基类（承载生命等通用属性）
#include "HumanAttributeSet.generated.h" // UHT 生成头文件，必须置于最后

/**
 * 竞技生化网游：人类幸存者属性集
 *
 * 架构定位：
 *  - 继承 UGameAttributeSet 的全部通用属性（Health / MaxHealth），
 *    在其之上承载"只有人类阵营才拥有"的专有属性；
 *  - 生化幽灵专有属性由对称的 UZombieAttributeSet 承载，二者互不干扰，
 *    从而在"人类 → 生化幽灵"变异时可按需整体切换属性集。
 *
 * 热更新边界（竞技网游铁律）：
 *  - 本类只定义"不可热更的数据结构"——属性字段与复制声明；
 *  - 具体数值、成长曲线与触发规则一律由 GameplayEffect / 数据资产在 Lua 侧配置。
 *
 * 当前状态：契约骨架。
 *  - 人类专有属性尚未定稿，故本类暂不声明任何属性字段，仅确立类分层与扩展位置；
 *  - 待玩法数值确认后再补 UPROPERTY + GZ_ATTRIBUTE_ACCESSORS + OnRep 复制链路。
 *
 * TODO（待玩法确认后填入，仅示意候选，不代表最终设计）：
 *  - 冲刺耐力 Stamina / MaxStamina（冲刺、翻滚等动作的体力消耗与恢复）；
 *  - 感染抗性 InfectionResistance（被生化幽灵攻击后抵抗转化的概率/进度）；
 *  - 装备负重 LoadoutWeight（影响移动速度的装备重量）。
 */
UCLASS()
class ZOMBIEKING_API UHumanAttributeSet : public UGameAttributeSet
{
	GENERATED_BODY()

public:
	/**
	 * 构造函数：保持空实现。
	 * 人类专有属性的初始数值同样由"初始化 GameplayEffect"在服务器灌入，不在此写死，
	 * 以满足热更新调参要求。
	 */
	UHumanAttributeSet();
};
