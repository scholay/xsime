# Linux 桌面完整版对齐

2026-10-17：维护者授权 Linux 对齐当前 macOS 内置扩展、Buffer、Capsule、Mailbox 及核心插件。此计划取代原 Linux 仅维护范围；功能、源码、测试、桌面验收及公开发布分别记录。

GitHub 里程碑：https://github.com/scholay/xsime/milestone/1

**追加验收要求（2026-10-17）**：所有组件、设置和管理界面以当前 macOS 原生实现为基准做像素级对齐。相同内容、主题、逻辑尺寸与缩放下保存双端截图、并排图和差异图，逐项核对布局、间距、颜色、字体度量、图标、边框、圆角及交互状态；不以 GTK 默认样式代替产品样式。业务与状态迁移另用共享样例验证。详见 [视觉与行为对齐合同](VISUAL-PARITY.md)。任务总数仍为 30，此要求并入每项验收，不另开一轮视觉改造。

P0 优先完成插件基础、实时翻译和 AI API；P1 补齐其余核心模块；P2 完成最终桌面与安装包验收。每项实现时同步运行相关回归，不把测试全部推迟至最后。

| 顺序 | 优先级 | GitHub 任务 | 状态 |
|---|---|---|---|
| 01 | P0 | [#151 插件注册、能力与启停生命周期](https://github.com/scholay/xsime/issues/151) | 已合入，CI 与审查通过 |
| 02 | P0 | [#152 官方插件安装、更新和完整性校验](https://github.com/scholay/xsime/issues/152) | 已合入，CI 与审查通过 |
| 03 | P0 | [#153 统一设置、插件配置与凭据存储](https://github.com/scholay/xsime/issues/153) | 基础已合入，模块页及视觉验收中 |
| 04 | P0 | [#154 Buffer 双轨工作区与异步任务状态](https://github.com/scholay/xsime/issues/154) | 进行中 |
| 05 | P0 | [#155 Linux 本地翻译引擎和资源管理](https://github.com/scholay/xsime/issues/155) | 引擎与资源管理已实现，视觉验收中 |
| 06 | P0 | [#156 实时增量翻译与连续输入](https://github.com/scholay/xsime/issues/156) | 增量翻译与宿主验证通过，验收中 |
| 07 | P0 | [#157 派生结果投递、源消费与自动发送](https://github.com/scholay/xsime/issues/157) | 原生投递与计时测试通过，视觉待验收 |
| 08 | P0 | [#158 AI API 插件与流式请求](https://github.com/scholay/xsime/issues/158) | HTTP 与 Fcitx 测试通过，外部渠道及视觉待验收 |
| 09 | P0 | [#159 保护态、锁屏睡眠与会话生命周期](https://github.com/scholay/xsime/issues/159) | D-Bus 与 Fcitx 保护测试通过，桌面及视觉待验收 |
| 10 | P0 | [#160 全局快捷键与跨输入法唤起](https://github.com/scholay/xsime/issues/160) | X11、portal 与 Fcitx 测试通过，Mailbox 接入及桌面视觉待验收 |
| 11 | P0 | [#161 桌面窗口、主题与多屏交互](https://github.com/scholay/xsime/issues/161) | 共享浮窗、缩放与工具栏测试通过，实际桌面及像素对照待验收 |
| 12 | P1 | [#162 Capsule 五模块宿主、管理页和底栏](https://github.com/scholay/xsime/issues/162) | 模块、存储、管理页与 Fcitx 测试通过，四主题截图校准中 |
| 13 | P1 | [#163 Capsule 结构化笔记与编辑](https://github.com/scholay/xsime/issues/163) | 结构化笔记、富文本及真实 Fcitx 测试通过，像素对照待验收 |
| 14 | P1 | [#164 Capsule 临时剪贴板历史](https://github.com/scholay/xsime/issues/164) | 原始 MIME、X11/Wayland 与宿主测试通过，四主题像素对照待验收 |
| 15 | P1 | [#165 Capsule 资源、文献和文件预览](https://github.com/scholay/xsime/issues/165) | 资源、引用与安装测试通过，四主题像素对照待验收 |
| 16 | P1 | [#166 Capsule 截图、录屏与捕获归档](https://github.com/scholay/xsime/issues/166) | 捕获、归档、宿主及 deb 测试通过，真实桌面录屏与像素待验收 |
| 17 | P1 | [#167 Capsule 密码库、结构化秘密与 TOTP](https://github.com/scholay/xsime/issues/167) | 待开发 |
| 18 | P1 | [#168 Mailbox 窗口、模块宿主和持久化](https://github.com/scholay/xsime/issues/168) | 待开发 |
| 19 | P1 | [#169 Mailbox 收件、附件与 Capsule/Buffer 流转](https://github.com/scholay/xsime/issues/169) | 待开发 |
| 20 | P1 | [#170 本地 MCP/HTTP 网关与收件审核](https://github.com/scholay/xsime/issues/170) | 待开发 |
| 21 | P1 | [#171 ChatGPT/Codex CLI 渠道插件](https://github.com/scholay/xsime/issues/171) | 待开发 |
| 22 | P1 | [#172 Claude Code CLI 渠道插件](https://github.com/scholay/xsime/issues/172) | 待开发 |
| 23 | P1 | [#173 Mailbox AI 对话工作区](https://github.com/scholay/xsime/issues/173) | 待开发 |
| 24 | P1 | [#174 Mailbox PTY 终端与手动成果收取](https://github.com/scholay/xsime/issues/174) | 待开发 |
| 25 | P1 | [#175 意识流输入插件与本地/AI 候选](https://github.com/scholay/xsime/issues/175) | 待开发 |
| 26 | P1 | [#176 并击扩展与自定义键位](https://github.com/scholay/xsime/issues/176) | 待开发 |
| 27 | P1 | [#177 日常输入统计与隐私聚合](https://github.com/scholay/xsime/issues/177) | 待开发 |
| 28 | P1 | [#178 打字测速与文章练习](https://github.com/scholay/xsime/issues/178) | 待开发 |
| 29 | P2 | [#179 Linux 桌面、宿主和跨模块验收](https://github.com/scholay/xsime/issues/179) | 待开发 |
| 30 | P2 | [#180 deb 生命周期、同版本发布集成与文档](https://github.com/scholay/xsime/issues/180) | 待开发 |

本轮不包含五个额外选装插件（Polisher、LaTeX、Reference、摩斯、电音）和跨设备内容同步。已有 #43 桌面退出验收继续独立跟踪；#44 已关闭。Linux deb 沿用项目 v* 标签与同一个 Release，新增持久化数据必须在升级与卸载中保留。

任务定义及依赖见 `PARITY-TASKS.json`。功能任务只有在对应条件有证据后才关闭；原生测试、容器桌面、实际设备及公开安装包的证据不可互相代替。
