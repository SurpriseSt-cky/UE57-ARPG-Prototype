// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "CharaInfo.generated.h"

class ABattleCharacter;
class UTexture2D;

/**
 * 角色信息表（Data_chara_imfor）的行结构。
 *
 * ★ 怎么用（0 基础版）：
 *   Content 右键 → Other（其他）→ Data Table（数据表）→ 行结构选
 *   「Chara Info Entry」→ 命名 Data_chara_imfor（放在 /Game/UI/chara_imf/ 下，
 *   C++ 默认从这里懒加载；也可在 BP_PlayerCharacter 的 Chara Info Table 里显式指定）。
 *   然后每行填一个角色：
 *     · 行名（Row Name）：角色的唯一标识（编队数据里记的就是它）
 *     · Chara Class     ：该角色的蓝图类（如 BP_PlayerCharacter / BP_Chara_magic）
 *     · Chara Name      ：显示名（如 绯雪）
 *     · Head Icon       ：头像图（角色面板小头像 + 快速编队网格用）
 *     · Portrait        ：立绘大图（编队界面立绘槽用；留空则用头像拉伸显示）
 *
 * ★ 谁在读这张表：
 *   ① 角色面板（WBP_Character_imf）chara_pitc_head 下的头像按钮 —— 按「拥有的角色」自动生成
 *   ② 编队界面（L 键）—— 立绘槽 / 快速编队角色列表
 *   两处共用一份表 = 「角色列表内容同步」；新增角色 = 表里加一行，两处自动出现。
 */
USTRUCT(BlueprintType)
struct WUTHERINGWAVES_API FCharaInfoEntry : public FTableRowBase
{
    GENERATED_BODY()

    /** 该角色的蓝图类（切人 / 上阵用；读 CDO 拿等级、攻击力等默认属性） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chara")
    TSubclassOf<ABattleCharacter> CharaClass = nullptr;

    /** 显示名（面板名称 / 编队立绘槽下方名字） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chara")
    FText CharaName;

    /** 头像图（角色面板头像按钮 + 快速编队网格） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chara")
    TObjectPtr<UTexture2D> HeadIcon = nullptr;

    /** 立绘大图（编队立绘槽；留空 → 用头像显示） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chara")
    TObjectPtr<UTexture2D> Portrait = nullptr;
};

/**
 * 一支队伍的成员（3 个行名；None = 该槽位为空）。
 *
 * 为什么包一层 USTRUCT 而不是直接 TArray<TArray<FName>>：
 *   UPROPERTY 不支持嵌套容器，编队数据要存进蓝图（可在 BP_PlayerCharacter 里预配队伍），
 *   必须是一层可编辑结构体。
 * MemberRows 固定 3 个元素（下标 0~2 = 界面上从左往右）；
 * C++ 侧 SetTeamMembers 会自动补 None / 截断到 3 个。
 */
USTRUCT(BlueprintType)
struct WUTHERINGWAVES_API FCharaTeamMembers
{
    GENERATED_BODY()

    /** 成员角色行名（对应 Data_chara_imfor 的行名；None = 空） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CharaTeam")
    TArray<FName> MemberRows;
};
