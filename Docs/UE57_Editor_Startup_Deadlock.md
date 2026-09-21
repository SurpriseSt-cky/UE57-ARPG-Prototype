# UE 5.7 编辑器启动死锁事故复盘与开发规范

> 事故日期：2026-09-08 ~ 2026-09-09
> 影响：编辑器完全无法启动，卡在 72% 加载界面约 20 小时
> 根因：`BattleCharacter.cpp` 构造函数同步加载复杂 Widget 蓝图
> 状态：已修复并验证通过

---

## 一、事故现象

| 项 | 表现 |
|----|------|
| 启动卡点 | 72% 加载界面（每次卡在不同模块：IKRig / AnimationModifierLibrary / Interchange） |
| CPU 占用 | 0% ~ 0.2%（死锁，非死循环） |
| 崩溃报告 | 无（进程挂起，未崩溃） |
| 日志 | 停在某一行后完全不再输出 |
| 绕过尝试 | `-NoDDC` / `-NoLoadStartupPackages` / `-nullrhi` / `-nosound` 全部无效 |

---

## 二、根本原因

### 2.1 直接原因

`Source/WutheringWaves/Private/BattleCharacter.cpp` 第 153 行（原）：

```cpp
// 构造函数中
static ConstructorHelpers::FClassFinder<UUserWidget> CharacterPanelClassFinder(
    TEXT("Blueprint'/Game/UI/WBP_Character_imf.WBP_Character_imf_C'"));
```

`ConstructorHelpers::FClassFinder` 会**同步阻塞**加载目标资产。而 `WBP_Character_imf` 是：

- 109KB 的复杂 Widget 蓝图
- 依赖 `Content/UI/chara_imf/` 下 8 个子资产
- 创建于 2026-09-08 20:17（C 键角色面板功能）

在编辑器启动的 **CDO（Class Default Object）构造阶段**做同步 IO，主线程陷入死锁。

### 2.2 时间线吻合

| 日期 | 事件 | 编辑器状态 |
|------|------|-----------|
| 09-07 | 正常运行 | 可启动 |
| 09-08 20:17 | 新增 C 键角色面板 + 构造函数加载 | **无法启动** |
| 09-09 12:24 | 改为懒加载后重新编译 | 可启动 |

成功日志（09-07 备份）中 `CharacterPanelClass` 关键字出现 **0 次** —— 证明当时这段代码根本不存在。

### 2.3 定位过程的关键证据

关闭 `r.GenerateMeshDistanceFields` 后，卡点**前移**，暴露了真正的阻塞点：

```
修改前日志最后一行：LogMeshUtilities: Finished distance field build  ← 误判为距离场问题
修改后日志最后一行：LogTemp: HealthBarWidgetClass loaded in constructor!  ← 卡点前移
```

对照构造函数代码顺序：

| 行号 | 加载内容 | 卡死日志输出 | 成功日志(09-07) |
|------|---------|-------------|----------------|
| 119 | WBP_StaminaBar | 有输出 | 有输出 |
| 140 | WBP_HealthBar | 有输出（**最后一行**） | 有输出 |
| **153** | **WBP_Character_imf** | **0 次输出 ← 死锁点** | **0 次（代码不存在）** |
| 158 | PoisonIcon 纹理 | 未执行到 | 有输出 |

---

## 三、走过的弯路（6 次误判）

| # | 误判方向 | 为何排除 |
|---|---------|---------|
| 1 | D 盘空间不足 | 剩余 107GB，充足 |
| 2 | 音频设备（XAudio2）死锁 | 修好 `AudioMaxChannels=0` 后仍卡（此配置确实有错，但不是卡死原因） |
| 3 | Zen Server / Turnkey UAT | UAT 单独运行 1 秒成功，ExitCode=0 |
| 4 | 僵尸进程 + LiveCodingConsole 残留 | 彻底清理后 `First instance` 干净启动，仍卡 |
| 5 | Shader Format SM5 vs SM6 | 改为 SM6 后仍卡（SM6 是正确配置，但非根因） |
| 6 | 距离场异步构建 | 关掉后卡点前移，证明它只是"最后一个可见操作" |

**教训**：所有这些都是"相关性"而非"因果性"。真正的突破口是**让卡点前移**——主动关掉一个可疑项，看卡死位置是否变化。

---

## 四、后续开发规范（务必遵守）

### 4.1 代码红线

#### 禁止：构造函数同步加载项目自定义资产

```cpp
// 禁止
AMyCharacter::AMyCharacter()
{
    static ConstructorHelpers::FClassFinder<UUserWidget> Finder(
        TEXT("Blueprint'/Game/UI/WBP_Panel.WBP_Panel_C'"));   // 同步 IO，可能死锁
}
```

#### 推荐：懒加载

```cpp
void AMyCharacter::OpenPanel()
{
    if (!PanelClass)
    {
        PanelClass = LoadClass<UUserWidget>(nullptr,
            TEXT("/Game/UI/WBP_Panel.WBP_Panel_C"));
    }
    // ...使用
}
```

#### 判定标准

| 资产类型 | 构造函数加载 | 说明 |
|---------|------------|------|
| 引擎内置小资源（`/Engine/EngineMaterials/...`） | 允许 | 已常驻内存 |
| 项目自定义 UI（Widget Blueprint） | **禁止** | 用 `LoadClass` 懒加载 |
| 项目自定义纹理 / 材质 | **禁止** | 用 `TSoftObjectPtr` 异步加载 |
| 简单蓝图（<20KB，无子资产依赖） | 谨慎 | 优先仍用懒加载 |

### 4.2 本项目现存隐患（建议后续改造）

`BattleCharacter.cpp` 中仍有 4 处构造函数同步加载，目前未触发死锁但存在同样风险：

| 行号 | 资产 | 建议 |
|------|------|------|
| 119 | `/Game/UI/WBP_StaminaBar` | 改为 `BeginPlay` 懒加载 |
| 140 | `/Game/UI/WBP_HealthBar` | 改为 `BeginPlay` 懒加载 |
| 223 | `/Game/UI/WBP_HurtScreen` | 改为使用时懒加载 |
| 236 | `/Game/UI/WBP_Skill` | 改为使用时懒加载 |

### 4.3 新增功能的自检清单

在提交任何 C++ 改动前，逐项确认：

- [ ] 构造函数中**没有** `ConstructorHelpers::FClassFinder` / `FObjectFinder` 加载项目自定义资产
- [ ] 新增的 Widget 类引用走 `LoadClass` 懒加载路径
- [ ] 新增 `.cpp` 文件后**重新生成项目文件**（UBT 否则不会纳入编译）
- [ ] 编译完成后确认 DLL 时间戳已更新
- [ ] 启动编辑器验证能进主窗口

### 4.4 编辑器卡死的排查顺序（高效版）

**第 1 步：看日志最后一行**（30 秒）

```
tail -5 Saved/Logs/WutheringWaves.log
```

**第 2 步：对照代码顺序**（关键）

拿最后一行日志去 `grep` 源码，找到它对应的代码位置，**下一行就是死锁点**。

```
grep -n "HealthBarWidgetClass loaded" Source/WutheringWaves/Private/*.cpp
```

**第 3 步：让卡点前移**

临时关闭最后一个可疑的 CVar（如 `r.GenerateMeshDistanceFields=False`），重启看卡点是否变化。变化了就说明真正的问题在更前面。

**第 4 步：清理进程**（排除干扰）

```powershell
Stop-Process -Name "UnrealEditor","CrashReportClientEditor","LiveCodingConsole","zenserver" -Force
```

注意：`LiveCodingConsole.exe` 是独立进程，关闭编辑器窗口**不会**让它退出，是常见的隐形干扰源。项目根目录有 `Cleanup_UE_Processes.bat` 可一键清理。

**第 5 步：对比历史成功日志**

```
Saved/Logs/WutheringWaves_2-backup-*.log
```

---

## 五、本次的附带修复

| 配置项 | 修改前 | 修改后 | 说明 |
|--------|-------|-------|------|
| `D3D12TargetedShaderFormats` | `PCD3D_SM5` | `PCD3D_SM6` | 09-07 成功时本就是 SM6，RTX 4060 支持 SM 6.7 |
| `D3D11TargetedShaderFormats` | `PCD3D_SM5` | `PCD3D_SM6` | 同上 |
| `AudioMaxChannels` | `0` | `32` | 0 会导致音频混音器分配 0 通道 |
| `r.GenerateMeshDistanceFields` | `True` | `True`（排查时临时改 False，已恢复） | Lumen GI 依赖，保持开启 |

---

## 六、编译命令备忘

编辑器必须完全关闭后执行：

```bash
cd /d/UE_5/UE_5.7/Engine/Build/BatchFiles
./Build.bat WutheringWavesEditor Win64 Development \
  -Project="D:/U_5_project/WutheringWaves/WutheringWaves.uproject" \
  -NoUBA -DisableUnrealBuildAccelerator
```

注意：`-Project` 参数**必须带**，否则报 `Couldn't find target rules file`。
