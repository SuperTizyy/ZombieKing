#include "HumanAttributeSet.h"

UHumanAttributeSet::UHumanAttributeSet()
{
	// 刻意保持空实现：
	// 人类专有属性的初始数值将由"初始化 GameplayEffect"在服务器生成角色时统一灌入，
	// 保证改数值只需改 Lua 侧配置，无需重新编译 C++（满足竞技网游热更新要求）。
	//
	// TODO（后续迭代）：当人类专有属性定稿后，在此处或初始化 GE 中完成数值灌入与委托绑定。
}
