# 角色头像格子（WBP_CharaHead_Slot）与编队 UI（WBP_CharaTeam）创建步骤

> 本文档给两份 UI 资产的**点击级**创建步骤 + 控件命名约定。
> 核心原则（与背包系统一致）：**格子/界面 = 独立 Widget，C++ 按约定名取控件填内容**，
> 控件名写错 → C++ 打日志报错并回退，不会静默白屏。

---

## 一、前提

1. 已重启编辑器（本轮新增了 `CharaHeadSlotClass` / `CharaTeamWidgetClass` 两个字段和 `UCharaTeamWidget` 的 WBP 接入）。
2. 已建好数据表 `Data_chara_imfor`（行结构 `CharaInfoEntry`），每行填了角色蓝图类 + 头像（`HeadIcon`）。
   - 若还没建表，头像列表 / 编队立绘会回退旧数组或显示占位，不影响本文档步骤本身。

---

## 二、创建头像格子蓝图 WBP_CharaHead_Slot

这个格子对应 `chara_pitc_head` 里的**一个头像按钮**，与背包的 `WBP_Bag_Slot` 同构。

### 2.1 新建 Widget Blueprint

1. 内容浏览器 → 定位到 `Content/UI` 目录（没有就右键新建文件夹 `UI`）。
2. 右键空白处 → **用户界面** → **控件蓝图（Widget Blueprint）**。
3. 命名：**`WBP_CharaHead_Slot`**（必须这个名字，C++ 默认按此路径懒加载）。
   - 路径必须是 `/Game/UI/WBP_CharaHead_Slot`。

### 2.2 设计格子内部控件

双击打开 `WBP_CharaHead_Slot`，在**设计器**里搭下面这个结构：

```
[根] Overlay  (命名 root_head_slot)
  ├─ Image     (命名 img_head_icon)      ← 头像图（铺满）
  └─ Image     (命名 img_select_frame)   ← 金色选中框（铺满，默认隐藏）
```

**控件名（必须一字不差）：**

| 控件名 | 类型 | 作用 | 说明 |
|--------|------|------|------|
| `img_head_icon` | Image | 角色头像 | 铺满格子；C++ 用 `SetBrushFromTexture` 填 |
| `img_select_frame` | Image | 选中框 | 铺满；默认 `Hidden`，选中时 C++ 置 `Visible` |

> ⚠️ 也可以不设根 Overlay，直接把两个 Image 平铺；但建议根用 Overlay 让两个 Image 叠放。

### 2.3 设置选中框的描边样式（可选但推荐）

选中 `img_select_frame`，在 **细节（Details）** 面板：

1. **Brush** → **Draw As** 设为 **Border（边框）**。
2. **Brush** → **Outline Settings** → **Color** 设为金色（如 `R=0.92, G=0.78, B=0.25`）。
3. **Brush** → **Outline Settings** → **Width** 设为 `3`。
4. **Brush** → **Tint** 的 Alpha 设为 `0`（中间透明，只留描边）。

这样选中框就是一圈金边，不挡头像。

### 2.4 尺寸

- 格子**不需要**自己设死尺寸：C++ 会在外层套 `USizeBox`（64×64）钉死，防止塌成 0×0。
- 但建议根 Overlay 的锚点设为**铺满（Fill）**，让头像和选中框填满整个 64×64 格子。

### 2.5 保存

Ctrl+S 保存。到此头像格子完成，无需在格子蓝图里写任何蓝图逻辑（C++ 负责填充 + 绑定点击）。

---

## 三、创建编队 UI 蓝图 WBP_CharaTeam

编队界面（L 键打开）本轮新增了 **WBP 接入点**：
- **建了 WBP 并在设计器里搭好控件** → 用你的设计树；
- **没建 WBP 或控件没配全** → C++ 自动回退纯代码构建，功能照常（不会白屏）。

### 3.1 新建 Widget Blueprint

1. 内容浏览器 → `Content/UI` 目录 → 右键 → **用户界面** → **控件蓝图**。
2. 命名：**`WBP_CharaTeam`**（必须这个名字）。
3. **关键（L 键打不开 WBP 的根因就在这）**：右下角 **Class Settings（类设置）** → **父类（Parent Class）** 选 **`CharaTeamWidget`**。
   - 这样它才继承 `UCharaTeamWidget` 的逻辑（队伍数据、点击分发、逐位写回都在父类里）。
   - ⚠️ **父类必须设成 `CharaTeamWidget`，不能是默认的 `UserWidget`**。如果父类仍是 `UserWidget`，
     C++ 里 `Cast<UCharaTeamWidget>` 会失败 → 丢掉你的 WBP、回退纯 C++ 界面，
     表现为「按 L 打开了界面，但不是你做的 WBP」（或误以为没打开）。
   - 改法：选中 WBP → 工具栏「类设置」→ 详情面板顶部的「父类」下拉 → 搜 `CharaTeamWidget` 选它 → 编译保存。

### 3.2 搭控件（控件名必须一字不差）

主视图核心控件（**缺 `txt_team_title` 或任一个 `slot_0/1/2` → C++ 回退纯代码构建**）：

| 控件名 | 类型 | 作用 |
|--------|------|------|
| `txt_team_title` | TextBlock | 左上「队伍 N」标题 |
| `slot_0` / `slot_1` / `slot_2` | Overlay | 中间 3 个立绘槽（必须 3 个都在） |

快速编队视图控件（**缺了只会让「快速编队」功能降级，主视图照常**）：

| 控件名 | 类型 | 作用 |
|--------|------|------|
| `btn_quick_edit` | Button | 右下「快速编队」按钮 |
| `grid_pick` | UniformGridPanel | 快速编队角色网格 |
| `btn_finish` | Button | 快速编队「完成」按钮 |
| `quick_edit_root` | CanvasPanel | 快速编队覆盖层（默认 `Hidden`） |

关闭按钮（**本轮新增，配了就会自动绑点击**）：

| 控件名 | 类型 | 作用 |
|--------|------|------|
| `close_line_butt` | Button | 关闭**整个编队界面**（回游戏，等同再按一次 L） |
| `close_edit_butt` | Button | 关闭**快速编队覆盖层**（回主视图，不关整个界面） |

立绘槽 `slot_0/1/2` 内，各自放 3 个**带下标**的子控件（下标从 0 开始）：

| 控件名 | 类型 | 作用 |
|--------|------|------|
| `img_portrait_0`（在 slot_0 内） | Image | 立绘/头像 |
| `txt_level_0`（在 slot_0 内） | TextBlock | Lv |
| `txt_name_0`（在 slot_0 内） | TextBlock | 名字 |
| `img_portrait_1` / `txt_level_1` / `txt_name_1`（在 slot_1 内） | … | slot_1 的立绘/Lv/名字 |
| `img_portrait_2` / `txt_level_2` / `txt_name_2`（在 slot_2 内） | … | slot_2 的立绘/Lv/名字 |

> ⚠️ **立绘槽子控件必须带下标**（`img_portrait_0` 而不是三个槽都叫 `img_portrait`），
> 因为 C++ 是全局按名查控件，重名会永远只取到第一个槽。

可选控件（不配也能跑，只是少了队伍切换）：

| 控件名 | 类型 | 作用 |
|--------|------|------|
| `btn_team_0` ~ `btn_team_7` | Button | 左侧 8 个队伍号按钮，每个按钮下挂一个 TextBlock 显示数字 |

### 3.3 搭结构示意

```
[根] Canvas Panel  (铺满)
  ├─ TextBlock     txt_team_title           ← 标题
  ├─ Button        btn_team_0  (下挂 TextBlock "1")
  ├─ Button        btn_team_1  (下挂 TextBlock "2")
  ├─ ... (到 btn_team_7)
  ├─ Overlay       slot_0  → Image img_portrait_0 + TextBlock txt_level_0 + TextBlock txt_name_0
  ├─ Overlay       slot_1  → Image img_portrait_1 + TextBlock txt_level_1 + TextBlock txt_name_1
  ├─ Overlay       slot_2  → Image img_portrait_2 + TextBlock txt_level_2 + TextBlock txt_name_2
  ├─ Button        btn_quick_edit
  ├─ Button        close_line_butt          ← 关闭整个编队界面（本轮新增）
  └─ CanvasPanel   quick_edit_root  (默认 Hidden)
        ├─ UniformGridPanel  grid_pick
        ├─ Button            btn_finish
        └─ Button            close_edit_butt  ← 关闭快速编队（本轮新增）
```

### 3.4 保存

Ctrl+S 保存。到此编队 UI 完成。

---

## 四、验证

1. **C 键**打开角色面板 → 看 `chara_pitc_head` 是否按拥有角色生成头像格子；点头像 → 该头像出现金框、左侧属性同步。
2. **切 mod_butt 页签**（角色/武器/装备/命座）→ 角色页、武器页都显示**当前选中角色**的信息（选别人时武器页显示别人的默认武器，选自己时显示自己的装备）。
3. **L 键**打开编队 → 若建了 WBP_CharaTeam 且控件配全，用的是你的设计树；否则自动回退纯代码界面。
4. 日志前缀：`[CharaHead]`（头像格子）、`[Team]`（编队）、`[Arm]`（武器页）。

---

## 五、控件名写错的后果（为什么不能猜）

- 头像格子 `img_head_icon` / `img_select_frame` 写错 → 头像/选中框不显示，但**不报崩溃**（C++ 按名取不到就跳过该项）。
- 编队 WBP 核心控件缺一个 → C++ **打日志并回退纯代码构建**，界面照常可用（只是不是你设计的那版）。
- 立绘槽子控件重名（都叫 `img_portrait`）→ 三个槽永远显示同一个头像，这是最容易看错的坑。

> 排查时先看输出日志里有没有 `[CharaHead]` / `[Team]` 前缀的提示，它会把「缺哪个控件名」直接点出来。
