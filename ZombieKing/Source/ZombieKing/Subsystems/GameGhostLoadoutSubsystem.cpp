#include "GameGhostLoadoutSubsystem.h"
#include "GameCharacterBase.h"             // 幽灵角色基类（默认清单元素类型）
#include "ZombieBasicCharacter.h"          // 基础幽灵（免费保底角色）
#include "ZombieMotherCharacter.h"         // 压制型母体
#include "ZombieCrawlerCharacter.h"        // 多足爬墙怪（Character/zombie 为独立 include 根）
#include "ZombieKing.h"

UGameGhostLoadoutSubsystem::UGameGhostLoadoutSubsystem()
{
    // -------------------------------------------------------------------------
    // 默认仓库（测试期全解锁）：基础幽灵（免费保底）+ 压制型母体 + 多足爬墙怪。
    //  - 顺序即选择框展示顺序与"索引选择"的基准顺序（见头文件说明：正式运营后只追加不重排）；
    //  - 本属性标记 Config=Game：内容侧在 DefaultGame.ini 里配置 [/Script/ZombieKing.GameGhostLoadoutSubsystem]
    //    段的 OwnedGhosts 即可整表覆盖（例如加入蓝图角色类），无需改 C++；
    //  - ini 覆盖发生在 CDO 阶段：若内容侧配置了本属性，此处构造期填入的默认值会被 ini 值替换。
    // -------------------------------------------------------------------------
    OwnedGhosts = {
        AZombieBasicCharacter::StaticClass(),
        AZombieMotherCharacter::StaticClass(),
        AZombieCrawlerCharacter::StaticClass(),
    };
}

bool UGameGhostLoadoutSubsystem::IsGhostOwned(TSubclassOf<AGameCharacterBase> GhostClass) const
{
    // 空类不算拥有（与 PlayerState 侧校验同一口径）
    if (GhostClass == nullptr)
    {
        return false;
    }

    return OwnedGhosts.Contains(GhostClass);
}
