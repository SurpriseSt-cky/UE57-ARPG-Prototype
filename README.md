# Wuthering Waves ARPG Prototype (UE 5.7)

> 基于 **Unreal Engine 5.7** 开发的《鸣潮》风格 ARPG 战斗原型。
> 纯 **C++ + GAS** 实现核心战斗系统，UMG 负责 UI 层。
>
> 中文版见下方 | [English version below](#english-version)

---

## 📖 项目简介

这是一个以**动作战斗手感**为研究目标的 ARPG 原型。项目不是《鸣潮》的资源复刻，而是**用 C++ 从零实现**其战斗系统的核心机制：

- **切人连携系统** —— 1/2/3 键即时换人，支持"技能中留场 / 普攻中衔接 / 地面继承速度"三种形态
- **完美闪避（Perfect Dodge）** —— 无敌帧 + 残影特效 + 屏幕反馈
- **伤害结算系统** —— 基于 GAS 的 ExecutionCalculation，含暴击、等级系数、护盾吸收
- **武器 / 背包系统** —— per-class 装备表、唯一实例占用、职位约束三层校验
- **自研相机** —— 不依赖 `bUsePawnControlRotation`，独立 `CameraWorldYaw` 基准

> ⚠️ **本项目不包含美术资源。** Content 目录中的第三方素材（Epic Paragon、Dragon8 等约 7 GB）因**版权限制未上传**，详见 [美术资源](#-美术资源必读) 章节。

---

## ✨ 功能特性

### 战斗系统

| 模块 | 说明 |
|---|---|
| **伤害结算** | `总攻 × 倍率 × (1+增伤) × [暴击伤害] × 等级系数 × 0.9`；等级系数 = `(100+角色级) / (199+角色级+怪物级)`；护盾优先吸收 |
| **伤害飘字** | 独立屏幕 WidgetComponent，含上飘 / 淡出 / 散布；普通与暴击两套样式 |
| **完美闪避** | 判定窗口内闪避触发无敌帧（`InvincibilityGameplayEffect`）+ 残影（`GhostAfterimage`） |
| **招架 / 击退 / 起身** | 受击状态机 + Montage 驱动 |
| **4 处伤害结算点** | 普攻分段 / 下落攻击 / 武器 / 投射物，全部 `ApplyPointDamage` → `MonsterBase::TakeDamage` |

### 角色与切换

| 模块 | 说明 |
|---|---|
| **切人系统** | `RequestSwitchToTeamSlot` → 队伍表 → `Data_chara_imfor` → spawn + Possess |
| **三种切换形态** | 技能中（留场放完自毁）/ 普攻中（衔接窗口内）/ 其余（原地继承 Velocity） |
| **玩家级状态跨实例共享** | `static TMap` 承载 CD 表，用 `FPlatformTime::Seconds()` 计时（跨 PIE 安全） |
| **装备继承** | `TransferPlayerStateTo` 在 Possess **前**调用，装备恢复在之后 |

### 背包 / 武器

| 模块 | 说明 |
|---|---|
| **装备表** | `static TMap<UClass*, TSubclassOf<AWeaponBase>>`（**不写 CDO**，否则子类串号） |
| **唯一实例** | `FBagItemEntry.InstanceNo` + `WeaponInstanceOwner`（key = 行名#实例号） |
| **职位约束** | `EJobClass` → `EWeaponCategory` 三层校验 |
| **蓝图武器自动发现** | 按 `PackagePaths` + `IsChildOf` 扫描（**不是**按父类筛，那样恒 0） |

### UI（UMG）

- 角色面板（`C` 键）—— 头像格、编队、武器筛选
- 背包（`B` 键）—— 格子动态列表、拾取提示
- 编队（`L` 键）—— `UCharaTeamWidget` + `WBP_CharaTeam`

---

## 🎮 操作说明

### 移动 / 视角

| 按键 | 功能 |
|---|---|
| `W` `A` `S` `D` | 移动（方向基准 = 相机朝向） |
| `鼠标` | 转动视角 |
| `Shift` | 疾跑（消耗耐力） |
| `Ctrl`（左/右） | 切换步行 |
| `空格` | 跳跃 |
| `Backspace` | 相机复位到背后视角（自动寻怪朝向） |

### 战斗

| 按键 | 功能 |
|---|---|
| `鼠标左键` | 普通攻击（多段连击） |
| `鼠标右键` | 闪避（完美闪避判定） |
| `Q` / `E` | 技能 / 能量技能 |
| `R` | 大招 |

> 具体按键以 `Config/DefaultInput.ini` 与蓝图中的 Enhanced Input 资产为准。

### 界面 / 系统

| 按键 | 功能 |
|---|---|
| `1` `2` `3` | 切换队伍成员（切人） |
| `C` | 角色面板 |
| `B` | 背包 |
| `L` | 编队界面 |

---

## 🔧 环境要求

| 项目 | 版本 |
|---|---|
| **Unreal Engine** | **5.7.x**（本项目在 5.7.4 上开发） |
| **Visual Studio** | 2022（需 `Desktop development with C++` + `Game development with C++`） |
| **Windows SDK** | 10.0.22621 或更高 |
| **MSVC 工具链** | 14.38 / 14.44 均可 |
| **.NET** | UE 自带 bundled dotnet 8.0.412（无需单独安装） |

### 引擎插件依赖

以下插件需在引擎中启用（`.uproject` 已声明）：

- `ModelingToolsEditorMode`
- `GameplayAbilities` ← **GAS，核心依赖**
- `EnhancedInput`
- `ModularGameplay`
- `GameFeatures`
- `DataRegistry`
- `Niagara`
- `CommonUI`
- `AnimationWarping`
- `VisualStudioTools`

---

## 🚀 快速开始

### 1. 克隆仓库

```bash
git clone https://github.com/<你的用户名>/WutheringWaves.git
cd WutheringWaves
```

### 2. 准备美术资源（必需）

**仓库不含美术资源**，clone 后需要自行获取，否则角色 / 怪物会显示为缺省网格体。
详见 [美术资源](#-美术资源必读) 章节。

### 3. 生成 VS 工程文件

- **方式 A（推荐）**：右键 `WutheringWaves.uproject` → `Generate Visual Studio project files`
- **方式 B（命令行）**：
  ```bash
  "<UE安装路径>/Engine/Build/BatchFiles/GenerateProjectFiles.bat" ^
    -project="%CD%\WutheringWaves.uproject" -game -rocket -progress
  ```

### 4. 编译

```bash
"<UE安装路径>/Engine/Build/BatchFiles/Build.bat" WutheringWavesEditor Win64 Development ^
  -Project="%CD%\WutheringWaves.uproject" -WaitMutex -NoUBA
```

> 💡 **`-NoUBA` 建议始终加上**。UE 的 UBA 执行器在部分环境下会因文件占用报
> `Access is denied`，进而导致链接失败并**删掉已有的 DLL**。

### 5. 打开工程

双击 `WutheringWaves.uproject`。首次打开会：
- 编译着色器（可能需要 10–30 分钟）
- 加载 `/Game/ww_begin_map` 作为默认关卡

### 6. 若资源缺失导致报错

编译期见到大量 `找不到 <资产名>` 警告属正常（美术资源未提供）。
只要**关卡能打开**，就可以用引擎自带的 Mannequin 临时替换角色网格体验证代码逻辑。

---

## 📁 项目结构

```
WutheringWaves/
├── Config/                     # 引擎 / 输入 / 游戏配置
│   ├── DefaultEngine.ini       # 默认关卡、渲染设置、类重定向
│   ├── DefaultGame.ini         # 项目 ID、AssetManager 扫描规则
│   └── DefaultInput.ini        # 轴映射（Enhanced Input 资产为主）
│
├── Source/
│   ├── WutheringWaves.Target.cs         # Game target
│   ├── WutheringWavesEditor.Target.cs   # Editor target
│   └── WutheringWaves/
│       ├── WutheringWaves.Build.cs      # 模块依赖
│       ├── Public/                      # 头文件（23 个）
│       └── Private/                     # 实现（20 个）
│
├── Docs/                       # 设计与踩坑文档（中文）
├── Plugins/                    # VisualStudioTools（IDE 集成）
└── WutheringWaves.uproject
```

### 核心类一览

| 类 | 职责 |
|---|---|
| `ABattleCharacter` | 玩家主控角色（约 12000 行）：输入、移动、技能、切人、背包、面板 |
| `AMonsterBase` / `ABossSting` | 怪物基类 / Boss |
| `AWeaponBase` | 武器基类（挂载到 `hand_r` 的 `WeaponSocket`） |
| `AProjectileBase` | 投射物基类（发射点复用 `GetMuzzleLocation`） |
| `UBattleAttributeSet` | GAS 属性集（生命 / 攻击 / 防御 / 暴击 / 护盾） |
| `UDamageExecutionCalculation` | 伤害结算（GE Execution） |
| `UDodgeAbility` / `UltimateAbility` | 闪避 / 大招 GameplayAbility |
| `AGhostAfterimage` | 闪避残影 |
| `UInventoryComponent` | 背包数据层 |
| `UCharaTeamWidget` | 编队 UI |
| `UBagTypes` | 背包数据结构与广播接口 |

---

## 🎨 美术资源（必读）

### ⚠️ 为什么仓库里没有美术资源

本仓库采用 **「源码优先」策略**。`Content/` 目录中约 **7.4 GB** 的素材来自第三方，
**其授权协议（Epic EULA 等）通常不允许再分发到公开仓库**，因此被 `.gitignore` 排除。

### 需要自行准备的素材

| 素材包 | 体积 | 来源 | 用途 |
|---|---|---|---|
| **ParagonSparrow / Kwang / Wraith** | 6.7 GB | Epic Games（免费，Fab 商店或 Epic 启动器 → 学习 → Paragon 系列） | 角色模型 / 材质 / 动画 |
| **Mannequins**（`SKM_Manny_Simple` 等） | — | **随引擎自带**，路径 `Engine/Content/Characters/Mannequins` | 默认角色骨架 |
| **Dragon8** | 330 MB | 第三方商店 | 怪物模型 |
| **Whisper** | 100 MB | 第三方商店 | 特效 |
| **Rapier_AnimSet / RamsterZ_FreeAnims** | 85 MB | 第三方商店 / Fab | 武器动画 |
| **MedievalSword** | 18 MB | Fab 商店 | 武器模型 |

### 导入位置

获取后放到以下路径（**保持目录名一致**，否则蓝图引用会断）：

```
Content/
├── ParagonSparrow/
├── ParagonKwang/
├── ParagonWraith/
├── Dragon8/
├── Whisper/
├── Rapier_AnimSet/
├── RamsterZ_FreeAnims_Volume1/
├── MedievalSword/
└── characters/Mannequins/
```

> 💡 若只想**跑通编译、不追求画面**，可以**完全不装**这些素材 ——
> 代码编译不依赖任何美术资产。打开关卡后把角色蓝图里的 Mesh 换成引擎自带
> `SKM_Manny_Simple` 即可测试战斗逻辑。

---

## 🐛 常见问题

<details>
<summary><b>编译报 <code>LNK1136: 文件无效或损坏</code> / <code>LNK1201</code>，且 DLL 消失了</b></summary>

**原因**：UBA 执行器无法写入被占用的文件（常见于编辑器 / VS 还开着）。

**处理**：
1. 关掉 UnrealEditor 和 VS
2. 删掉 0 字节的 `.lib` / `.exp` / `.pdb`
3. 加 `-NoUBA` 重新编译

</details>

<details>
<summary><b>编译时报 <code>C1083: 无法打开 CoreMinimal.h</code>，且<b>每个文件</b>都报</b></summary>

**原因**：几乎总是**工作目录错误**，不是真的缺引擎头文件。
UBT 生成的 `.Shared.rsp` 里存的是相对路径（`Runtime/Core/Public`），
必须在 `<引擎>/Engine/Source` 目录下执行才会有正确的相对基准。

**处理**：正常用 `Build.bat` 编译不会遇到。若你手工调 `cl.exe`，把 cwd 设为 `<引擎>/Engine/Source`。

</details>

<details>
<summary><b>所有文件都失败、单个文件 0.2 秒就"编译完"、没有具体 error C####</b></summary>

**原因**：**磁盘空间不足**。cl.exe 根本没启动起来，UBT 只看到 `error code 1`。

**处理**：
```bash
df -h                # 确认剩余空间
set TEMP=D:\ue_build_temp
set TMP=D:\ue_build_temp
```
把临时目录重定向到空闲盘后重试。

</details>

<details>
<summary><b>中文日志 / UI 显示为乱码</b></summary>

**原因**：源文件被保存为 **GBK**（中文 Windows 下 VS 的默认编码）。
UE 5.x 的 `VCToolChain.cs` 会给 cl.exe 强制加 `/utf-8`，于是 GBK 中文被当成非法 UTF-8，
**编译不报错，运行时乱码**。

**处理**：把 `.h/.cpp` 转成 **UTF-8 无 BOM**。

```python
import pathlib
p = pathlib.Path("Source/WutheringWaves/Private/Xxx.cpp")
raw = p.read_bytes()
text = raw.decode("gb18030")
new = text.encode("utf-8")
# 安全校验：ASCII 字节必须逐位不变（GBK 是 ASCII 兼容编码）
assert bytes(b for b in raw if b < 0x80) == bytes(b for b in new if b < 0x80)
p.write_bytes(new)
```

</details>

<details>
<summary><b>打开关卡提示 <code>缺少 HiHookAnchor 类</code> 或其它已删除的类</b></summary>

**原因**：关卡 / 蓝图里残留了已被删除的 Actor 引用。

**处理**：打开关卡，在 World Outliner 里找到那个红色（缺失）的 Actor 删掉，保存即可。

</details>

---

## 📚 相关文档

`Docs/` 目录下记录了开发过程中的设计决策与踩坑：

| 文档 | 内容 |
|---|---|
| `职位系统_职位与武器类别.md` | `EJobClass` → `EWeaponCategory` 的三层约束设计 |
| `角色编队与角色信息表.md` | `Data_chara_imfor` 表结构、切人链路、显示名三级兜底 |
| `角色头像格子与编队UI创建步骤.md` | UMG 动态列表的按钮回调载体模式、格子尺寸塌陷问题 |
| `UE57_Editor_Startup_Deadlock.md` | UE 5.7 编辑器启动死锁排查记录 |

---

## 📄 授权协议

本项目**源代码**采用 [MIT License](LICENSE) 授权。

**注意**：MIT 协议仅覆盖本仓库中的**源代码与文档**。
`Content/` 目录下的美术资源（若你自行获取并放入）**不受本协议约束**，
请遵守其各自的原始授权条款（Epic EULA 等）。

《鸣潮》（Wuthering Waves）是 **库洛游戏（Kuro Games）** 的注册商标。
本项目为**个人学习 / 技术研究**用途，与库洛游戏无任何关联，不使用其官方素材。

---

## 🙏 致谢

- **Epic Games** —— Unreal Engine 5.7、Paragon 系列免费素材、Mannequin 角色
- **库洛游戏** —— 《鸣潮》提供的战斗系统设计灵感
- 所有第三方动画 / 模型 / 特效素材的原作者

---

<br>

---

<a id="english-version"></a>

# Wuthering Waves ARPG Prototype (UE 5.7)

> A Wuthering-Waves-style **ARPG combat prototype** built on **Unreal Engine 5.7**.
> Core combat systems implemented in pure **C++ with GAS**; UI layer in UMG.

## 📖 Overview

This project is a **combat-feel research prototype**, not an asset copy of Wuthering Waves.
It reimplements the core mechanics of that game's combat system from scratch in C++:

- **Character Switch Combo System** — instant switching via `1`/`2`/`3`, with three distinct
  transition states (mid-skill leaves the previous character on field / mid-normal-attack
  chain window / ground state inherits velocity)
- **Perfect Dodge** — invincibility frames + afterimage VFX + screen feedback
- **Damage Calculation** — GAS `ExecutionCalculation` with crit, level scaling, shield absorption
- **Weapon / Inventory System** — per-class equipment table, unique instance claiming,
  three-layer job-class constraints
- **Custom Camera** — standalone `CameraWorldYaw` basis, not relying on `bUsePawnControlRotation`

> ⚠️ **No art assets are included.** Roughly 7 GB of third-party content (Epic Paragon,
> Dragon8, etc.) is **excluded due to licensing restrictions**. See [Art Assets](#-art-assets-required).

## ✨ Features

### Combat

| Module | Description |
|---|---|
| **Damage Formula** | `ATK × Mult × (1+Bonus) × [CritDMG] × LevelFactor × 0.9`; `LevelFactor = (100+ActorLv) / (199+ActorLv+MonsterLv)`; shields absorb first |
| **Damage Numbers** | Per-hit screen-space `WidgetComponent` with float-up / fade-out / scatter; separate normal & crit styles |
| **Perfect Dodge** | Invincibility GE (`InvincibilityGameplayEffect`) + afterimage (`GhostAfterimage`) inside the timing window |
| **Parry / Knockback / Getup** | Hit-reaction state machine driven by montages |
| **4 Damage Application Points** | Normal attack combo / plunge attack / weapon / projectile — all route through `ApplyPointDamage` → `MonsterBase::TakeDamage` |

### Characters & Switching

| Module | Description |
|---|---|
| **Switch Pipeline** | `RequestSwitchToTeamSlot` → team table → `Data_chara_imfor` → spawn + `Possess` |
| **Three Transition Modes** | Mid-skill (previous stays and self-destructs) / mid-attack (combo window) / default (in-place velocity inherit) |
| **Cross-Instance Player State** | `static TMap` cooldown table timed with `FPlatformTime::Seconds()` (PIE-safe) |
| **Equipment Carry-Over** | `TransferPlayerStateTo` invoked **before** `Possess`; equipment restore after |

### Inventory / Weapons

| Module | Description |
|---|---|
| **Equipment Table** | `static TMap<UClass*, TSubclassOf<AWeaponBase>>` — deliberately **not** on the CDO (would leak across subclasses) |
| **Unique Instances** | `FBagItemEntry.InstanceNo` + `WeaponInstanceOwner` (key = `RowName#InstanceNo`) |
| **Job Constraints** | `EJobClass` → `EWeaponCategory`, three-layer validation |
| **Blueprint Weapon Discovery** | Scans via `PackagePaths` + `IsChildOf` (**not** by parent-class filter, which always yields 0) |

### UI (UMG)

- Character Panel (`C`) — portrait grid, team setup, weapon filtering
- Inventory (`B`) — dynamic slot list, pickup toasts
- Team Setup (`L`) — `UCharaTeamWidget` + `WBP_CharaTeam`

## 🎮 Controls

### Movement / Camera

| Key | Action |
|---|---|
| `W` `A` `S` `D` | Move (camera-relative) |
| `Mouse` | Look |
| `Shift` | Sprint (stamina cost) |
| `Ctrl` (L/R) | Toggle walk |
| `Space` | Jump |
| `Backspace` | Reset camera behind character (auto-faces nearest target) |

### Combat

| Key | Action |
|---|---|
| `LMB` | Normal attack (multi-hit combo) |
| `RMB` | Dodge (perfect-dodge window) |
| `Q` / `E` | Skill / Energy skill |
| `R` | Ultimate |

> Exact bindings live in `Config/DefaultInput.ini` and the Enhanced Input assets in blueprints.

### UI / Systems

| Key | Action |
|---|---|
| `1` `2` `3` | Switch party member |
| `C` | Character panel |
| `B` | Inventory |
| `L` | Team setup |

## 🔧 Requirements

| Item | Version |
|---|---|
| **Unreal Engine** | **5.7.x** (developed on 5.7.4) |
| **Visual Studio** | 2022 with `Desktop development with C++` + `Game development with C++` |
| **Windows SDK** | 10.0.22621 or newer |
| **MSVC Toolchain** | 14.38 / 14.44 both fine |
| **.NET** | Bundled dotnet 8.0.412 shipped with UE (no separate install) |

### Required Engine Plugins

Declared in the `.uproject`:

`ModelingToolsEditorMode`, `GameplayAbilities` (**core GAS dependency**), `EnhancedInput`,
`ModularGameplay`, `GameFeatures`, `DataRegistry`, `Niagara`, `CommonUI`,
`AnimationWarping`, `VisualStudioTools`

## 🚀 Getting Started

### 1. Clone

```bash
git clone https://github.com/<your-name>/WutheringWaves.git
cd WutheringWaves
```

### 2. Obtain art assets (required)

The repo ships **without art assets**; fetch them yourself or characters/monsters will
appear as default meshes. See [Art Assets](#-art-assets-required).

### 3. Generate VS project files

- **Option A (recommended)**: right-click `WutheringWaves.uproject` → `Generate Visual Studio project files`
- **Option B (CLI)**:
  ```bash
  "<UE_ROOT>/Engine/Build/BatchFiles/GenerateProjectFiles.bat" ^
    -project="%CD%\WutheringWaves.uproject" -game -rocket -progress
  ```

### 4. Build

```bash
"<UE_ROOT>/Engine/Build/BatchFiles/Build.bat" WutheringWavesEditor Win64 Development ^
  -Project="%CD%\WutheringWaves.uproject" -WaitMutex -NoUBA
```

> 💡 **Always pass `-NoUBA`.** UE's UBA executor can hit an `Access is denied` error
> under some environments, which fails the link step and **deletes the existing DLL**.

### 5. Open the project

Double-click `WutheringWaves.uproject`. First launch will compile shaders
(10–30 min) and load `/Game/ww_begin_map` as the default map.

### 6. If assets are missing

A flood of `missing asset` warnings during load is expected (assets not shipped).
As long as the **level opens**, you can swap character meshes to the engine's built-in
Mannequin to verify the code logic.

## 📁 Project Layout

```
WutheringWaves/
├── Config/                     # Engine / input / game config
├── Source/
│   ├── WutheringWaves.Target.cs
│   ├── WutheringWavesEditor.Target.cs
│   └── WutheringWaves/
│       ├── WutheringWaves.Build.cs
│       ├── Public/             # 23 headers
│       └── Private/            # 20 implementation files
├── Docs/                       # Design & troubleshooting notes (Chinese)
├── Plugins/                    # VisualStudioTools
└── WutheringWaves.uproject
```

### Key Classes

| Class | Responsibility |
|---|---|
| `ABattleCharacter` | Player character (~12k LOC): input, movement, abilities, switching, inventory, panels |
| `AMonsterBase` / `ABossSting` | Monster base / Boss |
| `AWeaponBase` | Weapon base (attached to `WeaponSocket` on `hand_r`) |
| `AProjectileBase` | Projectile base (reuses `GetMuzzleLocation` as spawn point) |
| `UBattleAttributeSet` | GAS attribute set (HP / ATK / DEF / Crit / Shield) |
| `UDamageExecutionCalculation` | Damage execution (GE Execution) |
| `UDodgeAbility` / `UltimateAbility` | Dodge / Ultimate GameplayAbilities |
| `AGhostAfterimage` | Dodge afterimage |
| `UInventoryComponent` | Inventory data layer |
| `UCharaTeamWidget` | Team setup UI |
| `UBagTypes` | Inventory structs & broadcast interfaces |

## 🎨 Art Assets (Required)

### ⚠️ Why no art assets are included

This repo follows a **source-first** policy. The ~7.4 GB of content under `Content/`
comes from third parties, and **their licenses (Epic EULA, etc.) generally prohibit
redistribution in public repositories**, so they are excluded via `.gitignore`.

### What you need to obtain

| Pack | Size | Source | Purpose |
|---|---|---|---|
| **ParagonSparrow / Kwang / Wraith** | 6.7 GB | Epic Games (free — Fab, or Epic Launcher → Learn → Paragon series) | Character meshes / materials / animations |
| **Mannequins** (`SKM_Manny_Simple`, etc.) | — | **Ships with the engine**: `Engine/Content/Characters/Mannequins` | Default character rig |
| **Dragon8** | 330 MB | Third-party marketplace | Monster meshes |
| **Whisper** | 100 MB | Third-party marketplace | VFX |
| **Rapier_AnimSet / RamsterZ_FreeAnims** | 85 MB | Third-party / Fab | Weapon animations |
| **MedievalSword** | 18 MB | Fab | Weapon mesh |

### Where to place them

Drop them at the following paths, **keeping directory names identical**
(otherwise blueprint references break):

```
Content/
├── ParagonSparrow/
├── ParagonKwang/
├── ParagonWraith/
├── Dragon8/
├── Whisper/
├── Rapier_AnimSet/
├── RamsterZ_FreeAnims_Volume1/
├── MedievalSword/
└── characters/Mannequins/
```

> 💡 If you only want to **compile and run the logic** without the visuals, you can
> **skip all of these** — the code has no art dependencies. After opening the level,
> swap the character blueprint's Mesh to the built-in `SKM_Manny_Simple` to test combat.

## 🐛 Troubleshooting

<details>
<summary><b><code>LNK1136: invalid or corrupt file</code> / <code>LNK1201</code>, and the DLL disappeared</b></summary>

**Cause**: the UBA executor can't write to locked files (usually editor / VS still running).

**Fix**:
1. Close UnrealEditor and VS
2. Delete the 0-byte `.lib` / `.exp` / `.pdb`
3. Rebuild with `-NoUBA`

</details>

<details>
<summary><b><code>C1083: cannot open CoreMinimal.h</code>, reported for <b>every single file</b></b></summary>

**Cause**: almost always a **wrong working directory**, not a missing engine header.
The generated `.Shared.rsp` uses relative paths (`Runtime/Core/Public`), so the
relative base must be `<Engine>/Engine/Source`.

**Fix**: normal `Build.bat` builds never hit this. If invoking `cl.exe` manually,
set cwd to `<Engine>/Engine/Source`.

</details>

<details>
<summary><b>Every file fails, each in 0.2 s, with no real <code>error C####</code></b></summary>

**Cause**: **out of disk space**. cl.exe never started; UBT only saw `error code 1`.

**Fix**:
```bash
df -h                # check free space
set TEMP=D:\ue_build_temp
set TMP=D:\ue_build_temp
```
Redirect temp files to a drive with free space and retry.

</details>

<details>
<summary><b>Chinese log / UI text shows as garbage</b></summary>

**Cause**: source files saved as **GBK** (the VS default on Chinese Windows).
UE 5.x forces `/utf-8` on cl.exe, so GBK Chinese becomes invalid UTF-8 —
**compiles fine, garbles at runtime**.

**Fix**: convert `.h/.cpp` to **UTF-8 without BOM**.

```python
import pathlib
p = pathlib.Path("Source/WutheringWaves/Private/Xxx.cpp")
raw = p.read_bytes()
new = raw.decode("gb18030").encode("utf-8")
# Safety: ASCII bytes must be byte-identical (GBK is ASCII-compatible)
assert bytes(b for b in raw if b < 0x80) == bytes(b for b in new if b < 0x80)
p.write_bytes(new)
```

</details>

<details>
<summary><b>Level reports <code>missing HiHookAnchor class</code> or another deleted class</b></summary>

**Cause**: a deleted Actor class is still referenced by the level or a blueprint.

**Fix**: open the level, locate the red (missing) Actor in the World Outliner,
delete it, and save.

</details>

## 📚 Documentation

`Docs/` contains design decisions and troubleshooting notes (in Chinese):

| Doc | Content |
|---|---|
| `职位系统_职位与武器类别.md` | `EJobClass` → `EWeaponCategory` three-layer constraint design |
| `角色编队与角色信息表.md` | `Data_chara_imfor` schema, switch pipeline, three-tier display-name fallback |
| `角色头像格子与编队UI创建步骤.md` | UMG dynamic-list button callback pattern, slot-size collapse issue |
| `UE57_Editor_Startup_Deadlock.md` | UE 5.7 editor startup deadlock investigation |

## 📄 License

The **source code** in this repository is licensed under the [MIT License](LICENSE).

**Note**: MIT covers only the **source code and documentation** in this repo.
Art assets under `Content/` (if you obtain and add them) are **not** covered —
observe their original license terms (Epic EULA, etc.).

*Wuthering Waves* is a registered trademark of **Kuro Games**.
This project is for **personal study / technical research** only, is not affiliated
with Kuro Games, and uses none of their official assets.

## 🙏 Credits

- **Epic Games** — Unreal Engine 5.7, Paragon free asset packs, Mannequin character
- **Kuro Games** — Wuthering Waves, for the combat-system design inspiration
- All third-party animation / mesh / VFX asset authors
