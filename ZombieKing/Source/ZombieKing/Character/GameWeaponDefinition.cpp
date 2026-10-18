#include "GameWeaponDefinition.h"

UGameWeaponDefinition::UGameWeaponDefinition()
{
    // 本资产为纯数据载体：构造期不做任何逻辑，也不需要创建子对象。
    // 所有默认值已由各 UPROPERTY 的成员初始值给出（改默认值 = 改 C++，属结构性调整；
    // 调枪感请在资产实例上覆盖，那样无需重编译）。
}

FPrimaryAssetId UGameWeaponDefinition::GetPrimaryAssetId() const
{
    // 固定类型名 + 资产自身名字，得到稳定的主资产标识（如 WeaponDefinition:DA_Weapon_Rifle）。
    // 必须覆写的原因见头文件；不覆写则类型会被推导为原生类名，与内容管线登记口径不一致。
    return FPrimaryAssetId(FPrimaryAssetType(TEXT("WeaponDefinition")), GetFName());
}

float UGameWeaponDefinition::GetFireIntervalSeconds() const
{
    // 射速换算：RPM → 秒/发。
    // 刻意不在这里做下限钳制：RPM <= 0 表达"不限制射速"（调试/特殊武器），此时间隔为 0，
    // 由武器组件按"无间隔"处理（见 UGameWeaponComponent 的节流实现）。
    return RoundsPerMinute > 0.0f ? (60.0f / RoundsPerMinute) : 0.0f;
}
