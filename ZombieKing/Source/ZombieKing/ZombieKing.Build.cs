// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ZombieKing : ModuleRules
{
	public ZombieKing(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate",
			// -----------------------------------------------------------------
			// 竞技生化玩法框架依赖
			// -----------------------------------------------------------------
			"Mover",              // Mover 2.0 运动框架（多足爬墙/双足行走统一驱动，UE5.7 Experimental 插件）
			"UnLua",              // Lua 脚本层（角色逻辑 C++ 极薄，表现与技能请求下沉到 Lua）
			"GameplayAbilities",  // GAS 技能系统（PlayerState 托管 ASC）
			"GameplayTags",       // 游戏标签（例如 Movement.Mode.Climbing 运动模式标签）
			"GameplayTasks",      // GAS 任务依赖（AbilityTask 基础模块）
			// -----------------------------------------------------------------
			// 自研后端（服务层）依赖
			// -----------------------------------------------------------------
			"HTTP",               // 引擎 HTTP 模块（后端传输网关：唯一出站口，见 UGameBackendSubsystem）
			"WebSockets",         // 引擎 WebSocket 模块（实时通道唯一出口，见 UGameRealtimeSubsystem）
			"Json",               // JSON 序列化（组装/解析后端报文）
			"DeveloperSettings"   // UDeveloperSettings（后端连接配置暴露到 Project Settings 并落到 ini，可热更）
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"ZombieKing",
			"ZombieKing/GameFramework",      // 对局框架层：GameState 等「局」级权威状态与复制契约
			"ZombieKing/GAS",                // GAS 属性层：通用属性集基类 + 人类/生化专有属性集
			"ZombieKing/Character",          // 通用角色基类 / GAS 玩家状态 / 生命与战斗组件
			"ZombieKing/Character/Animation", // 通用动画实例基类（Mover 同步状态 → 动画图 数据桥）
			"ZombieKing/Character/zombie",   // 多足生化角色：爬墙模式、吸附过渡、专属 Mover 组件与蝎子专属动画实例
			"ZombieKing/UI",                 // UI 层：HUD 控件基类（C++ 契约骨架，表现逻辑下沉 Lua）
			"ZombieKing/Audio",              // 音频层：音频事件表（标签→声音）与音频服务（音量/混音/播放收口）
			"ZombieKing/Subsystems",         // 服务层：会话/遥测/反作弊/设置/本地化/音频等服务子系统
			"ZombieKing/Variant_Horror",
			"ZombieKing/Variant_Horror/UI",
			"ZombieKing/Variant_Shooter",
			"ZombieKing/Variant_Shooter/AI",
			"ZombieKing/Variant_Shooter/UI",
			"ZombieKing/Variant_Shooter/Weapons"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
