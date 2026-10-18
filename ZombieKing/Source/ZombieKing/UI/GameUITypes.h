#pragma once

#include "CoreMinimal.h"
#include "GameUITypes.generated.h"

/**
 * 竞技生化网游：界面层级（5 层标准）
 *
 * 分层意义：界面不是"一堆各自 AddToViewport 的控件"，而是**分层管理**的 ——
 *  - ZOrder 按层分配固定区间，保证"后推入者在上"且层级关系永不错乱；
 *  - 只有"会接收输入的层"才让控件参与命中测试，背景层/提示层一律不挡操作（对局中的射击操作绝不能被 UI 吃掉）；
 *  - 输入模式（UIOnly / GameOnly）与鼠标显隐由"当前是否有交互界面"统一推导。
 */
UENUM(BlueprintType)
enum class EGameUILayer : uint8
{
    /** 背景层：主菜单背景、场景预览等纯展示内容（不接收输入） */
    Background UMETA(DisplayName = "背景层"),

    /** 主界面层：主菜单、房间、对局内 HUD 等主要界面（接收输入） */
    Window     UMETA(DisplayName = "主界面层"),

    /** 弹窗层：确认框、错误提示框等模态弹窗（接收输入） */
    Popup      UMETA(DisplayName = "弹窗层"),

    /** 覆盖提示层：飘字、加载遮罩、击杀提示等（不接收输入） */
    Overlay    UMETA(DisplayName = "覆盖提示层"),

    /** 调试层：GM 面板、调试信息（接收输入） */
    Debug      UMETA(DisplayName = "调试层")
};

/**
 * 层级布局规则（ZOrder 分配与输入策略）。
 * 放在头文件里用 inline 实现：规则是纯常量运算，无需 .cpp；多个模块引用也不会重复符号。
 */
namespace GameUILayerLayout
{
    /** 每层占用的 ZOrder 区间宽度（层内再按推入顺序递增，超出宽度的概率极低） */
    constexpr int32 ZOrderStride = 100;

    /** 层级的 ZOrder 基准值（层号 × 区间宽度） */
    inline int32 GetZOrderBase(EGameUILayer Layer)
    {
        return static_cast<int32>(Layer) * ZOrderStride;
    }

    /**
     * 该层是否接收键鼠输入。
     * 背景层与覆盖提示层**不接收**：它们只做展示，若参与命中测试会挡住对局操作（尤其 FPS 的射击/开火）。
     */
    inline bool DoesLayerCaptureInput(EGameUILayer Layer)
    {
        return Layer == EGameUILayer::Window
            || Layer == EGameUILayer::Popup
            || Layer == EGameUILayer::Debug;
    }
}
