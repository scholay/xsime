# XSIME 五端产品路线图

2026-10-19：按维护者确认的六条产品主线组织 29 项需求，与既有稳定性、兼容性和设备验收工作共用[原有 GitHub Project](https://github.com/users/scholay/projects/1)。本页记录产品方向；具体状态、讨论和验收证据以关联 Issue 为准。

五端为 macOS、Windows、Linux、iOS、Android。桌面三端与移动两端共享产品规范，分别实现宿主适配、权限和交互。沿用单一 `main`、短分支、独立构建与验收；规划、代码完成、设备验收和公开发布分别记录。

## 开发优先级

普通输入稳定性、方案能力配置与切换、候选和移动键盘体验优先；词库与短语新功能按独立范围推进，既有数据保护不变。实现约束见[系统架构](../architecture/system.md)，验收和发布门槛见[维护规则](../development/maintenance.md)。

以下主线表达产品方向，不代表本轮全部交付承诺。具体实现和验收状态以关联 Issue、代码及正式发布记录为准。

## 六条产品主线

| 主线 | 总单 | 范围 | 当前侧重 |
|---|---|---|---|
| A · 日常输入与 Buffer 交互 | [#229](https://github.com/scholay/xsime/issues/229) | 让日常输入、Buffer 唤起、编辑、发送及关闭形成一致且可验证的体验。 | 当前重点 |
| B · 输入方案与插件管理 | [#230](https://github.com/scholay/xsime/issues/230) | 统一方案和插件的发现、启停、配置、导入与管理入口。 | 下一阶段 |
| C · 个人词库与资料积累 | [#231](https://github.com/scholay/xsime/issues/231) | 明确个人内容成为候选、积累为词库、反馈纠错和长期保存的完整关系。 | 下一阶段 |
| D · AI 任务与 Mailbox | [#232](https://github.com/scholay/xsime/issues/232) | 将用户主动提交的 AI 任务、执行状态、完整结果和后续操作统一到 Mailbox 工作流。 | 下一阶段 |
| E · 五端品牌与视觉一致性 | [#233](https://github.com/scholay/xsime/issues/233) | 统一图标资源、品牌名称、官网及联系/群聊入口，核对实际客户端和分发材料。 | 当前重点 |
| F · 跨端传输与个人云服务 | [#234](https://github.com/scholay/xsime/issues/234) | 通过 Buffer + Mailbox 提供显式的文本/密码端到端加密传输、内容中继，以及官方和自部署服务。 | 后续探索 |

## 阶段与先后关系

- **当前重点**：持续处理稳定性与兼容性，验证 Buffer 首次唤起及 Linux/Fcitx5 共存；确定移动键盘、发送/关闭规则、设置入口、方案配置、AI 插件范式和 Mailbox 形态；开展个人词库研究与品牌/文档清理。
- **下一阶段**：按上述规范交付插件化入口、方案/词库导入、并击方案选择、辅助选重、语音/手写插件、AI 长按转交、电子书练习、资料候选触发及视觉对齐。跨端相同能力分别验收，不要求一轮完成全部平台。
- **后续探索**：明确中继协议和设备身份、密钥与存储边界后，开展 Buffer + Mailbox 加密传输、官方账号/云能力及开源 Docker 自部署。

阶段表示依赖和优先级，不是发布日期。设计/调研也是可交付任务：先写出可评审规范和结论，再据此拆实现。没有承诺日期，也没有自动把旧反馈全部提升为当前重点。

## 本轮需求与 Issue

原始第 4 项分为本地词库导入和联网接口研究；后者与第 21 项合并研究。语音复用 [#7](https://github.com/scholay/xsime/issues/7)，保留 [#109](https://github.com/scholay/xsime/issues/109) 反馈。以下 29 个工作项覆盖原始 29 项需求。

| 原始需求 | 主线 | 工作项 | 平台 | 阶段 |
|---|---|---|---|---|
| 1 | A | [#235](https://github.com/scholay/xsime/issues/235) [Buffer][PC] 剪贴板纯文本 hint 与点击粘贴 | 桌面端 | 下一阶段 |
| 2 | A | [#236](https://github.com/scholay/xsime/issues/236) [Buffer][PC] 首次唤起位置与响应修复 | 桌面端 | 当前重点 |
| 5 | A | [#237](https://github.com/scholay/xsime/issues/237) [移动端][设计] 键盘底栏、空格长度及两段式交互 | 移动端 | 当前重点 |
| 11 | A | [#238](https://github.com/scholay/xsime/issues/238) [Buffer][五端][设计] 主动／被动发送与最后一块上屏后关闭规则 | 跨平台 | 当前重点 |
| 3 | B | [#239](https://github.com/scholay/xsime/issues/239) [Buffer][移动端] 四个固定快捷入口改为可配置插件 | 移动端 | 下一阶段 |
| 12 | B | [#240](https://github.com/scholay/xsime/issues/240) [Buffer][PC] 设置图标、功能与插件配置统一入口 | 桌面端 | 当前重点 |
| 13 | B | [#241](https://github.com/scholay/xsime/issues/241) [输入方案][五端] 链接、鼠标移入与本地选取导入 | 跨平台 | 下一阶段 |
| 14 | B | [#242](https://github.com/scholay/xsime/issues/242) [并击][PC] 内置扩展开启时在输入方案列表常显 | 桌面端 | 下一阶段 |
| 15 | B | [#243](https://github.com/scholay/xsime/issues/243) [输入方案][五端] 按导入文件和方案能力提供配置项 | 跨平台 | 当前重点 |
| 16 | B | [#244](https://github.com/scholay/xsime/issues/244) [Mailbox][PC] 插件统一管理与配置 | 桌面端 | 下一阶段 |
| 4（本地导入） | C | [#245](https://github.com/scholay/xsime/issues/245) [词库][PC] 统一词库导入与失败诊断 | 桌面端 | 下一阶段 |
| 4（联网接口）、21 | C | [#246](https://github.com/scholay/xsime/issues/246) [调研] 个人联网词库场景与公开接口方案 | 跨平台 | 当前重点 |
| 17 | C | [#247](https://github.com/scholay/xsime/issues/247) [Capsule][设计] 宿主产品定位与个人词库场景 | 桌面端 | 当前重点 |
| 18 | C | [#248](https://github.com/scholay/xsime/issues/248) [打字练习][PC] 以导入电子书为主要练习内容 | 桌面端 | 下一阶段 |
| 19 | C | [#249](https://github.com/scholay/xsime/issues/249) [个人词库][设计] 输入统计、词库沉淀与错词淘汰联动 | 跨平台 | 当前重点 |
| 20 | C | [#250](https://github.com/scholay/xsime/issues/250) [个人词库][设计] RSS 与本地资料入库的候选触发 | 跨平台 | 下一阶段 |
| 9 | D | [#251](https://github.com/scholay/xsime/issues/251) [AI][五端] Buffer 长按提交 Mailbox 并等待完整结果 | 跨平台 | 下一阶段 |
| 10 | D | [#252](https://github.com/scholay/xsime/issues/252) [Mailbox][PC][设计] 右下角常驻小屏幕形态 | 桌面端 | 当前重点 |
| 6 | E | [#253](https://github.com/scholay/xsime/issues/253) [品牌][五端] 状态栏、App 与设置背景图标对齐 | 跨平台 | 下一阶段 |
| 8 | E | [#254](https://github.com/scholay/xsime/issues/254) [品牌][五端] 清理旧名称、官网和联系／群聊入口 | 跨平台 | 当前重点 |
| 7 | F | [#255](https://github.com/scholay/xsime/issues/255) [跨端][五端] Buffer + Mailbox 文本与密码端到端加密传输 | 跨平台 | 后续探索 |
| 22 | F | [#256](https://github.com/scholay/xsime/issues/256) [跨端][协议] 内容中继、接续发送与去重 | 跨平台 | 后续探索 |
| 23 | F | [#257](https://github.com/scholay/xsime/issues/257) [服务] 官方账号鉴权、中继、云存储与开源 Docker 自部署 | 跨平台 | 后续探索 |
| 24 | B | [#258](https://github.com/scholay/xsime/issues/258) [AI][五端][设计] 可手工配置的自定义 Buffer 插件统一范式 | 跨平台 | 当前重点 |
| 25 | B | [#259](https://github.com/scholay/xsime/issues/259) [候选][五端][设计] 按笔画数量、音调与结构辅助选重 | 跨平台 | 下一阶段 |
| 26 | B | [#7](https://github.com/scholay/xsime/issues/7) [Buffer][五端] 语音输入插件 | 跨平台 | 下一阶段 |
| 27 | B | [#260](https://github.com/scholay/xsime/issues/260) [Buffer][五端] 手写输入插件 | 跨平台 | 下一阶段 |
| 28 | A | [#261](https://github.com/scholay/xsime/issues/261) [Bug][Linux] 验证与既有 Fcitx5 输入方案的冲突 | Linux | 当前重点 |
| 29 | E | [#262](https://github.com/scholay/xsime/issues/262) [Docs][五端] 更新 rimes-project-skills 名称、技能说明与关联文档 | 跨平台 | 当前重点 |

## 产品决策与依赖

- Buffer 承接输入、编辑与显式投递；剪贴板文本只作为空输入框的 hint，点击才粘贴，开始输入即放弃。首次唤起、主动/被动发送、最后一块上屏与关闭选项形成统一规则，见 [#235](https://github.com/scholay/xsime/issues/235)、[#236](https://github.com/scholay/xsime/issues/236)、[#238](https://github.com/scholay/xsime/issues/238)。
- 先统一 Buffer 插件范式与配置入口（[#258](https://github.com/scholay/xsime/issues/258)、[#240](https://github.com/scholay/xsime/issues/240)），再迁移移动端固定快捷入口（[#239](https://github.com/scholay/xsime/issues/239)），接入语音（[#7](https://github.com/scholay/xsime/issues/7)）与手写（[#260](https://github.com/scholay/xsime/issues/260)）。统一范式允许用户手工配置各自 AI 服务，不要求集中到官方 AI；移动端权限、后台生命周期与桌面 CLI 能力分别处理。
- AI 类型插件支持长按将任务交给 Mailbox 并等待完整结果（[#251](https://github.com/scholay/xsime/issues/251)）；Mailbox 桌面形态趋向右下角常驻小屏幕（[#252](https://github.com/scholay/xsime/issues/252)），插件管理统一（[#244](https://github.com/scholay/xsime/issues/244)）。完整返回、失败、取消与再次投递的行为在实现前定义。
- 输入方案导入（[#241](https://github.com/scholay/xsime/issues/241)）与方案能力配置（[#243](https://github.com/scholay/xsime/issues/243)）共同设计，保留用户所述“鼠标移入”入口，具体交互在设计中确认；移动端定义对应触控方式。并击扩展开启时作为常显方案（[#242](https://github.com/scholay/xsime/issues/242)）。笔画数量、音调、结构选重（[#259](https://github.com/scholay/xsime/issues/259)）以数据来源及方案适配研究为前置。
- Capsule 的宿主定位（[#247](https://github.com/scholay/xsime/issues/247)）围绕个人词库使用场景确定，与统计/沉淀/错词淘汰（[#249](https://github.com/scholay/xsime/issues/249)）、RSS 和本地资料触发（[#250](https://github.com/scholay/xsime/issues/250)）联动；联网词库的个人使用与公开接口统一研究（[#246](https://github.com/scholay/xsime/issues/246)），避免先锁定厂商或收费服务。
- 跨端中继（[#256](https://github.com/scholay/xsime/issues/256)）、端到端加密文本/密码传输（[#255](https://github.com/scholay/xsime/issues/255)）及账号/云/自部署（[#257](https://github.com/scholay/xsime/issues/257)）分开追踪并声明依赖。覆盖手机↔手机、手机↔电脑、电脑↔电脑；是否提供官方服务不改变用户可选择自部署的方向。现有本地凭据、保护态及精确目标投递边界在新设计验收前保持。
- 品牌统一依据 [docs/development/branding.md](../development/branding.md)，图标 [#253](https://github.com/scholay/xsime/issues/253)、旧品牌/地址/联系入口 [#254](https://github.com/scholay/xsime/issues/254)、开发技能名称和关联文档 [#262](https://github.com/scholay/xsime/issues/262) 分项验收；兼容标识、上游 Rime 名称和历史证据保留必要区分。

## 旧计划如何衔接

| 既有内容 | 本轮处理 |
|---|---|
| 移动端 [#3](https://github.com/scholay/xsime/issues/3)、Windows [#8](https://github.com/scholay/xsime/issues/8) 维护总单 | 继续承担平台稳定性/设备验收；产品设计与跨平台能力进入六条主线。 |
| 五端方案导入 [#4](https://github.com/scholay/xsime/issues/4)、方案反馈 [#140](https://github.com/scholay/xsime/issues/140)/#141 | [#4](https://github.com/scholay/xsime/issues/4) 已完成结论保留；新增入口与按方案配置由 [#241](https://github.com/scholay/xsime/issues/241)/#243 跟踪，不把新增范围算作旧功能回归。 |
| 语音 [#7](https://github.com/scholay/xsime/issues/7)、反馈 [#109](https://github.com/scholay/xsime/issues/109) | [#7](https://github.com/scholay/xsime/issues/7) 转为五端 Buffer 插件工作项并进入下一阶段；保留原提案与 [#109](https://github.com/scholay/xsime/issues/109) 来源。 |
| 已关闭或未计划内容，包括 [#6](https://github.com/scholay/xsime/issues/6) | 保留历史结论；本轮通过新 Issue 表达新范围，不批量重开。 |
| Linux 30 项 [#151](https://github.com/scholay/xsime/issues/151)—[#180](https://github.com/scholay/xsime/issues/180) | 保留编号、提交和验收证据；[#175](https://github.com/scholay/xsime/issues/175) 原暂缓结论不变。与 Buffer/Mailbox/设置/个人词库相关的界面项目补充新规范关联，设计确定后重定验收基线，避免继续复刻已改变的旧布局。 |
| Linux [#179](https://github.com/scholay/xsime/issues/179)/#180、[#207](https://github.com/scholay/xsime/issues/207) 与新 [#261](https://github.com/scholay/xsime/issues/261) | 继续记录实机/像素验收和独立缺陷；[#261](https://github.com/scholay/xsime/issues/261) 是 Fcitx5 冲突验证，尚未确认根因。 |
| 鸿蒙探索与其他未纳入的新功能反馈 | 保留原后续探索状态，不纳入本轮“五端”交付承诺。 |

## 看板与交付规则

Project 用“产品主线”表达归属，“阶段”表达顺序，“平台”表达实现/验收范围，“类型”表达工作性质。主题总单保留清单与依赖，执行卡片记录验收标准；跨主线事项只保留一个主归属，通过关联链接协作。

Status 沿用“待梳理 → 可认领 → 进行中 → 待验收 → 已完成”。只有实际验证支持时才推进状态；历史已完成卡片与 Issue 关闭状态对齐。缺陷先复现再修复；设计和调研以结论交付，不以功能上线作为其唯一完成条件。

每轮交付记录源提交、构建配置、测试命令、产物摘要、设备/宿主、通过项及未验证项。平台和能力清单不能替代验证证据；发布事实见 [docs/development/maintenance.md](../development/maintenance.md) 与版本发布记录。

## 历史：2026-09-29 平台基础与 2026-10-17 Linux 扩围

以下按原文保留当时的范围、环境和证据；其中“当前”“本轮”“尚未实现”均指记录日期，不表示今日产品现状。产品计划以上述 2026-10-19 决策为准，运行时事实以源码、各平台验证记录及 [docs/architecture/system.md](../architecture/system.md) 为准。
