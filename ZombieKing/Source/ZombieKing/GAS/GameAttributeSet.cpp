#include "GameAttributeSet.h"
#include "Net/UnrealNetwork.h"    // DOREPLIFETIME_CONDITION_NOTIFY 所依赖的复制注册宏
#include "GameplayEffectExtension.h" // FGameplayEffectModCallbackData 的完整定义（PostGameplayEffectExecute 形参所需；GameplayEffect.h 内仅前向声明该类型）

UGameAttributeSet::UGameAttributeSet()
{
	// 刻意保持空实现：
	// 初始数值必须由"初始化 GameplayEffect"在服务器生成角色时统一灌入，
	// 从而保证数值来源单一（改数值只需改 Lua 侧配置，无需重新编译 C++，满足竞技网游热更新要求）。
}

void UGameAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	// 先让基类注册其自身声明复制的成员，避免漏注册导致子类复制异常
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// 使用 DOREPLIFETIME_CONDITION_NOTIFY 组合宏，语义拆解如下：
	//   - COND_None        ：对全部连接（含模拟代理）复制，远端可据此做命中反馈与头顶血条；
	//   - REPNOTIFY_Always ：每次数值变化都触发 OnRep（而非仅"值确实改变时"），
	//                        因为 GAS 的预测/回滚会重算数值，必须保证表现层每次都能同步刷新。
	DOREPLIFETIME_CONDITION_NOTIFY(UGameAttributeSet, Health,    COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UGameAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
}

void UGameAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	// 先执行基类逻辑，保持引擎默认行为链完整
	Super::PreAttributeChange(Attribute, NewValue);

	// 生命值钳制到 [0, 生命上限]：杜绝基础值被改为负数或突破上限
	if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxHealth());
	}
	// 生命上限钳制到 >= 0：避免出现"负上限"导致后续所有以它为界的钳制失效
	else if (Attribute == GetMaxHealthAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
}

void UGameAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	// 先执行基类逻辑，保证引擎默认的结算后处理不被打断
	Super::PostGameplayEffectExecute(Data);

	// 服务器权威的最后闸门：GameplayEffect 结算完成后，再次把最终值钳制到合法区间
	if (Data.EvaluatedData.Attribute == GetHealthAttribute())
	{
		// 生命最终值钳制到 [0, 生命上限]（GetHealth() 返回的是已含修饰符的最终值）
		SetHealth(FMath::Clamp(GetHealth(), 0.0f, GetMaxHealth()));

		// 生命归零判定：此处只做"判定"，不在此写死死亡后的玩法结果——
		// 依据热更新原则，死亡后果（人类死亡→转化为生化幽灵 / 生化幽灵死亡→复活或销毁）
		// 属于会频繁调整的规则，必须下沉到 UnLua 脚本中处理。
		if (GetHealth() <= 0.0f)
		{
			// TODO（后续迭代 / 交由 Lua）：广播致死事件（击杀者、被击杀者、伤害来源），
			//   由 GameMode 或 Lua 规则脚本执行击杀结算、转化与回合判定。
		}
	}
}

void UGameAttributeSet::OnRep_Health(const FGameplayAttributeData& OldValue)
{
	// 引擎标准做法：GAMEPLAYATTRIBUTE_REPNOTIFY 会更新 GAS 内部缓存并派发属性变更委托，
	// 使客户端 UI/表现层收到与服务器一致的变化（漏调用会导致 OnAttributeChanged 不触发）。
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGameAttributeSet, Health, OldValue);

	// TODO（后续迭代 / 交由 Lua）：在此驱动血条、受击飘字、低血量心跳音等表现刷新。
}

void UGameAttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& OldValue)
{
	// 通知 GAS 记录属性变化，保证最大值变化也能被监听者感知
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGameAttributeSet, MaxHealth, OldValue);

	// TODO（后续迭代 / 交由 Lua）：按新上限重算血条比例并刷新表现。
}
