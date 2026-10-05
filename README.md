<div align="center">

# 多重打击（Multi Hit）

**Anomaly 插件 · 为《异环》(Neverness To Everness) 提供「倍攻」效果**

版本 `1.0.0`　·　插件 ID `anomaly.builtin.multi-hit-mini`　·　许可 [AGPL-3.0-only](https://www.gnu.org/licenses/agpl-3.0.html)

</div>

---

## 这是什么

一次命中结算，被**按同一组参数重复执行多次** —— 也就是俗称的「倍攻」。

倍率设成 N，一次结算就跑 N 次：小怪原本要两下、三下才倒，装上以后一下就能解决。

| 倍率 | 实测表现 |
| --- | --- |
| 2 | 有效但体感微弱 |
| 20 | 小怪从「两下」变成「一下」 |
| 50 | 大多数小怪一下解决（**官方建议上限**） |
| 100 | 不触发风控，但**明显偏卡** |
| 200 | **被服务器踢回登录界面** |

本插件是从头写的独立实现，与任何第三方插件无关联。

---

## 安装

### 1. 前置条件

- **Anomaly 宿主**：本插件是 Anomaly 的插件，不能单独运行。
- **游戏**：目前只支持《异环》(`nte`)，且针对特定游戏版本的特征码（见「注意事项」）。

### 2. 放入插件目录

宿主默认从 **`<游戏目录>\HT\Binaries\Win64\Anomaly\plugins\`** 加载插件 —— 也就是
`HTGame.exe` 同级目录下的 `Anomaly\plugins`。把整个 `MultiHit-1.0.0` 文件夹放进去即可：

```text
<游戏目录>\HT\Binaries\Win64\Anomaly\plugins\
  MultiHit-1.0.0\
    manifest.json
    plugin.dll
    （其余文件可选，宿主只按 manifest 里声明的 entry 找 dll）
```

> **这个位置可以改。** 宿主目录下的 `anomaly.ini` 用 `[Platform] PluginDirectory` 指定插件目录：
> 写**绝对路径**时直接采用，写相对路径（默认值就是 `plugins`）时以 `Anomaly` 目录为基准。
>
> ```ini
> [Platform]
> PluginDirectory=C:\Users\<你的用户名>\AnomalyPlugins
> ```
>
> 把插件目录放到用户可写的位置，就不必每次写游戏安装目录都提管理员权限。
> ⚠️ 但**同时只会加载这一个目录** —— 改掉之后，原先放在 `Anomaly\plugins` 里的插件就不再生效了。

### 3. ⚠️ 在 Profile 里登记两个符号（**必做，否则插件不会工作**）

本插件**不在代码里硬编码任何游戏地址**，而是从宿主加载的构建配置
`<游戏目录>\Anomaly\profiles\nte\nte-current.json` 里读取两条特征码，再交给宿主的签名服务解析。

如果那份文件里没有下面两个符号，插件会保持惰性（面板会写明原因），不会崩也不会乱猜地址。

把这两项加进 `nte-current.json` 的 `symbols` 对象：

```json
"nte.multi-hit.burst": {
  "module": "HTGame.exe",
  "section": ".text",
  "pattern": "48 85 D2 0F 84 41 01 00 00 57 48 83 EC 60 48 8B F9 4D 85 C0 0F 84 2B 01 00 00 48 89 5C 24 70 48",
  "resolve": {"kind": "direct"},
  "validators": ["address-in-module", "executable"],
  "requiredBy": ["anomaly.builtin.multi-hit"]
},
"nte.multi-hit.gate": {
  "module": "HTGame.exe",
  "section": ".text",
  "pattern": "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D 68 A9 48 81 EC D0",
  "resolve": {"kind": "direct"},
  "validators": ["address-in-module", "executable"],
  "requiredBy": ["anomaly.builtin.multi-hit"]
}
```

同时把 `nte.multi-hit` 登记成**可选特性**（这样特征码失效时只会让本插件不可用，不影响宿主其它能力）：

```json
"features": {
  "nte.multi-hit": ["nte.multi-hit.burst", "nte.multi-hit.gate"]
},
"optionalFeatures": [
  "nte.multi-hit"
]
```

> 该文件位于游戏安装目录，写入通常需要管理员权限。改完可用宿主的
> `anomaly-profile validate <path> nte` 校验一下。

### 4. 启用

进游戏 → 按 `Insert` 呼出 Anomaly 界面 → **Plugins** → 找到「多重打击」→ 启用。

> 启用状态记在 `<游戏目录>\Anomaly\config\plugin-enablement.json` 里，按插件 ID
> （`anomaly.builtin.multi-hit-mini`）记录，所以给插件文件夹改个名字不会丢启用状态。

---

## 使用

面板上只有两样东西，加一句话：

| 控件 | 说明 |
| --- | --- |
| **启用多重打击** | 总开关。**关闭时游戏行为与未装插件完全一致**（不会做任何拦截） |
| **每次结算几次（2-100）** | 倍率，默认 50。可直接输入数字 |
| 提示词 | 来自 `manifest.json` 的 `description`，见下 |

> 建议倍率控制在50以下，超过可能造成卡顿、被服务器踢等异象

改动**即时生效**，不需要重载插件或重启游戏。设置会持久化到
`%LOCALAPPDATA%\Anomaly\MultiHitMini.cfg`（两个键：`enabled` / `mult`）。

### 面板上的状态行

面板最后一行是插件的自检结果，也是排错的第一手信息：

| 显示 | 含义 |
| --- | --- |
| `正在等待游戏模块…` / `等待宿主服务就绪…` | 启动中，正常，稍等即可 |
| `已就绪（burst=0x… gate=0x…）` | **一切正常**：Profile 读到了、特征码匹配上了、两个钩子都装好了 |
| `已就绪   放大 N 次 / 追加结算 N 次` | 正在工作，显示累计放大次数 |
| `未找到游戏函数：Profile 缺少 nte.multi-hit.* 符号，或游戏已更新导致特征码失效` | 第 3 步没做，或游戏更新了 —— 插件保持惰性 |
| `挂钩失败（burst=0x… gate=0x…）` | 地址找到了但装钩失败，通常是别的插件已占用同一地址 |

---

## ⚠️ 注意事项

**1. 倍率越高，被服务器处理的概率越大 —— 这是本插件唯一的真实风险。**

服务端至少有两个独立的检查面：**单次伤害量级**与**事件频率**。实测 50 安全、100 可用但偏卡、
**200 会被踢回登录界面**。所以：

- 面板上限被锁在 **100**，请**不要**去改这个上限。
- 官方建议 **≤ 50**。
- 被踢属于账号层面的后果，**风险由使用者自负**。

**2. 只支持当前游戏版本。** 两条特征码是针对特定版本的 `HTGame.exe` 定位出来的。游戏大版本更新后
大概率失效 —— 失效时插件**不会崩溃**，只会保持惰性并在面板写明「未找到游戏函数…」，重新定位
特征码即可（可用 Anomaly 仓库的 `tools/rescan_profile_signatures.ps1`）。

**3. 不要与其它会钩同一批地址的插件同时启用。** 同一地址只能有一个钩子，先装的占位，另一个会
`挂钩失败`。

**4. 倍率高时会卡。** 100 倍实测明显掉帧，这是重放本身的开销，不是 bug。

**5. 关闭开关 = 游戏原样。** 插件在关闭状态下不会拦截或改写任何判定，方便你随时对照原版行为。

**6. 仅供学习与研究。** 请勿用于商业用途；在多人/联机内容中使用可能被判定为异常行为。

---

## 工作原理

（给想读源码的人看的，全部逻辑都在 `plugin.cpp` 里，约 500 行）

游戏里存在两个原生函数：

- **`burst`** —— 一次命中结算的入口，接收 5 个参数（x64 下前 4 个走 `rcx/rdx/r8/r9`，第 5 个在
  `[rsp+0x28]`）。
- **`gate`** —— `burst` 开头的节流判定：`if (!gate(...)) return;`。它返回 0 时，`burst` 会直接退出。

插件做的事：

1. 从 Profile 读出两条 `pattern`，经 `anomaly.interop.signature` 解析成实际地址；
2. 用 `anomaly.interop.hook` 钩住 `burst`：在 detour 里**先原样调用一次**，再用**同一组参数**
   重放 `N-1` 次；
3. 同时钩住 `gate`：**只在放大期间**强制返回 1（放行），否则原样转发 —— 这就是「关闭开关等于
   游戏原样」的实现方式；
4. 重放期间用**线程局部重入守卫**拦截自己触发的重放，避免指数级放大；
5. 重放期间持有宿主的 **回调租约**（`begin_callback` / `end_callback`），这样热重载或卸载插件时
   宿主会等这次调用退出，不会把代码页从脚下拆掉。

设计上的取舍：**宁可惰性，绝不猜地址**。Profile 读不到、特征码不匹配、装钩失败 —— 任何一步失败都
只是保持不工作并在面板写明原因。

---

## 文件说明

| 文件 | 大小 | 说明 |
| --- | --- | --- |
| `plugin.dll` | 361984 | 插件本体（x64） |
| `plugin.cpp` | 21901 | 完整源码，方便审阅 |
| `manifest.json` | 807 | 插件清单：ID、版本号、面板提示词、依赖的服务 |
| `README.md` | — | 本文件 |
| `LICENSE` | 34523 | AGPL-3.0 全文 |
| `package.sha256` | — | 上面除本清单外所有文件的 SHA-256 校验清单 |

SHA-256：

```text
1EBE8E9A331EFD11BCE906E1602962CE4FDFF309735EC1E977E4A36FED0267F8  plugin.dll
E8212791F35C044F9F2C65919948B72F5B53C582792F0753D0AEE3B8E2622CFE  plugin.cpp
0B70BB4F8905BF83D68BEF01B0652DD39EBADB16855D92861DCCDF6CCED5E014  manifest.json
8486A10C4393CEE1C25392769DDD3B2D6C242D6EC7928E1414EFFF7DFB2F07EF  LICENSE
```

用宿主自带工具校验整包（在本目录下执行）：

```powershell
anomaly-plugin validate .
# valid id=anomaly.builtin.multi-hit-mini version=1.0.0 entry=plugin.dll
```

> ⚠️ `package.sha256` 必须**恰好覆盖包内每个文件**。宿主在从插件源安装/更新时会逐条核对，
> 报 `package.sha256 does not cover the exact package content` 就说明包里增删过文件而没重新打包。
> 此时用 `anomaly-plugin pack .\MultiHit-1.0.0 --output <临时目录>` 重新生成一份即可。

> 版本号与面板提示词**只在 `manifest.json` 里维护**：插件启动时读取自己包内的清单，把
> `version` 拼到面板标题后面（显示为「多重打击 v1.0.0」），`description` 直接当作面板上的
> 提示词。改这两项不需要重新编译。

---

## 从源码构建

源码是 Anomaly 主仓库里的一个内建插件靶标（`plugins/MultiHitMini/`），依赖宿主的 SDK 与
`anomaly_add_plugin` 构建助手，所以需要先取到 [Anomaly](https://github.com/AnomalyNTE) 主仓库：

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-relwithdebinfo --target anomaly_builtin_multi_hit_mini --parallel
```

产物在 `.build/windows-vs2022/bin/RelWithDebInfo/builtin-plugins/MultiHitMini/`。

如果你想做成独立仓库，可参考
[Anomaly 插件模板](https://github.com/AnomalyNTE/Anomaly-Plugin-Template)。

---

## 常见问题

**Q：装好了、也启用了，但打怪伤害没变化？**
看面板状态行。若显示「未找到游戏函数…」，说明第 3 步（在 Profile 里登记符号）没做或没生效。

**Q：显示「挂钩失败」？**
多半是另一个插件已经钩住了同一个地址。禁用那个插件后重开游戏。

**Q：游戏更新后失效了怎么办？**
特征码需要重新定位，更新 Profile 里的两条 `pattern` 即可，插件本身不用改。

**Q：为什么上限是 100 而不是更高？**
因为 200 实测会被服务器踢下线。上限是保护用户，不是技术限制。

**Q：会不会封号？**
不知道，没有可靠数据。已知的唯一后果是**被服务器踢回登录界面**。请自行判断是否使用。

---

## 许可与免责

本项目以 [**GNU Affero General Public License v3.0（AGPL-3.0-only）**](https://www.gnu.org/licenses/agpl-3.0.html)
分发；完整条文见本目录下的 [`LICENSE`](LICENSE) 文件。

本插件仅供学习与研究使用。使用本插件可能违反游戏服务条款，并可能导致账号受到限制；一切后果由
使用者自行承担，作者不对任何账号损失或数据损坏负责。

---

<details>
<summary><b>English summary</b></summary>

**Multi Hit** is a plugin for the **Anomaly** host that gives "damage multiplier" behaviour in
*Neverness To Everness* (`nte`). It hooks the game's native hit-resolution function and replays it
with identical arguments N-1 extra times, while releasing the function's own throttle gate during
the replay. The total multiplier is capped at 100; values above ~50 may cause stutter and a value of
200 was observed to get the client kicked by the server. Addresses are never hard-coded: the plugin
reads two signatures (`nte.multi-hit.burst`, `nte.multi-hit.gate`) from the host build profile and
resolves them via `anomaly.interop.signature`. If the signatures are missing or stop matching after a
game update, the plugin stays inert and says so in its panel. Licensed AGPL-3.0-only. Use at your
own risk.

</details>
