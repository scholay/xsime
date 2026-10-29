# XSIME 五端架构

本文描述当前检出源码的职责与维护契约，不代表所有平台能力相同，也不代表已发布或在线最新状态。
文档入口见[文档导航](../README.md)，构建、验收和发行规则见[维护说明](../development/maintenance.md)。

## 输入主链路

五端都由原生输入接口接收按键或触控，交给平台桥接调用 librime，随后更新组字、候选与提交文本。
Rime 负责方案规则、词典、候选和学习；前端负责焦点、布局、事件路由、权限及文本交付。
普通输入不依赖 AI、Mailbox、网络网关或独立网页 UI。

```text
原生输入事件 → 平台控制器 → Rime 会话 → 组字 / 候选 / commit
                                   ├→ 当前宿主的原生文本接口
                                   └→ Buffer 暂存 → 目标与内容复验 → 原生文本接口
```

组字不是已提交文本，候选展示也不是提交凭据。失焦、输入源切换、保护态或会话失效都必须使旧投递权限失效。
同一次物理操作只产生一次有效输出；被输入法处理的事件不能同时泄漏给宿主再执行一次。

## 五端入口与交付接口

| 平台 | 源码入口 | 宿主与进程边界 |
|---|---|---|
| macOS | [RIMESController.swift](../../Sources/RimeBuffer/RIMESController.swift)、[CRimeBridge](../../Sources/CRimeBridge/) | InputMethodKit；控制器、引擎桥接、候选和工作台在同一输入法进程，每个 IMK 控制器拥有独立 Rime 会话。 |
| iOS | [KeyboardViewController.swift](../../platforms/ios/Keyboard/KeyboardViewController.swift)、[RimeMobile.mm](../../platforms/ios/Bridge/RimeMobile.mm) | 主 App 管设置/资源，键盘扩展通过 `UITextDocumentProxy` 向当前文档交付。 |
| Android | [RimesInputMethodService.java](../../platforms/android/app/src/main/java/org/scholay/rimes/android/RimesInputMethodService.java)、[rime_jni.cpp](../../platforms/android/native/rime_jni.cpp) | `InputMethodService` / `InputConnection`；引擎工作进入 `EngineWorker` 串行队列。 |
| Windows | [TextService.cpp](../../platforms/windows/native/src/tsf/TextService.cpp)、[broker/main.cpp](../../platforms/windows/native/src/broker/main.cpp) | TSF DLL 留在宿主中，Broker 管引擎及周边；宿主写入通过 TSF edit session。 |
| Linux | [rimes_ime.cpp](../../platforms/linux/ime/src/fcitx/rimes_ime.cpp)、[rime_engine.cpp](../../platforms/linux/ime/src/engine/rime_engine.cpp) | Fcitx5 addon 管输入上下文；通过当前 `InputContext` 更新 preedit、候选并 `commitString`。 |

macOS 的 [CompositionSession](../../Sources/RimeBuffer/CompositionSession.swift) 维持 marked text；全部普通提交、回退和 Buffer 投递经过 [Delivery.insert](../../Sources/RimeBuffer/Delivery.swift)。
iOS 的 [ProxyTextDelivery](../../platforms/ios/Support/ProxyTextDelivery.swift) 检查文档身份，并保持 marked-text 提交和后续编辑的顺序；不得用私有 API 推断宿主身份。
Android 的异步结果携带 editor lease，提交前复验相同 `InputConnection`；`commitText` 成功后才确认消费 Buffer 内容。
Windows 的 TSF 上下文、composition 和 Broker 会话不能互换；普通输入端点与 Buffer/设置控制权限分开，x64/x86 保持相同契约。
Linux 只处理自己的 Fcitx5 会话，保护、退出和部署取消不得影响其他输入法或写入其用户目录。

## 共享代码与资源

| 位置 | 职责与使用范围 |
|---|---|
| [Shared/Sources/RimesCore](../../Shared/Sources/RimesCore/) | macOS/iOS 共享的 Swift Buffer、键盘偏好、插件请求、AI 和输入习惯模型；不提供跨平台窗口实现。 |
| [Shared/InputHabits](../../Shared/InputHabits/) | 五端桥接共用的 C++ Rime switch 能力识别、显式覆盖与跨方案恢复规则。 |
| [Shared/SchemeImport](../../Shared/SchemeImport/) | Windows/Linux 共用导入实现；Swift、Java 前端按同一导入契约实现，不能假定五端共用此 C++ 库。 |
| [Shared/CandidateMatrix](../../Shared/CandidateMatrix/) | Windows/Linux 共用候选导航；展示预览不能永久改变实际引擎页。 |
| [Shared/AppUpdate](../../Shared/AppUpdate/) / [ProductDocuments](../../Shared/ProductDocuments/) | Windows/Linux 更新清单规则与嵌入式产品文档。 |
| [rime-data](../../rime-data/) / [Catalog](../../Catalog/) | 内置方案、资源来源及发行目录；用户词库和运行时数据另存。 |

Windows 原生工程中的 `RimesCore` CMake target 与 Swift 包同名，但不是同一实现。
五端通过行为、格式和回归样本对齐；字体、窗口、权限、触控与宿主 API 继续由各端实现。
[DesignSystem](../../DesignSystem/) 是 React 交互设计源，不能替代原生输入窗口或引入运行时 WebView 依赖。

## Buffer 的焦点与内容事务

Buffer 是输入法内的暂存与处理能力，不是 Rime 组字本身，也不是跨应用剪贴板注入通道。
macOS 的 [BufferModel](../../Sources/RimeBuffer/BufferModel.swift) 保存有稳定身份的块；[BufferWindowController](../../Sources/RimeBuffer/BufferWindowController.swift) 只负责工作台呈现与操作。
[InputFocusCoordinator](../../Sources/RimeBuffer/InputFocusCoordinator.swift) 管理单调递增的 `FocusToken` 和当前投递 lease；同一 App 的另一个文本框也属于不同目标。
[BufferDeliveryCoordinator](../../Sources/RimeBuffer/BufferDeliveryCoordinator.swift) 在插入前逐块重新取得源内容并验证目标，窗口不能私存客户端后直接写入。
以下工作台事务细节以 macOS 实现为参照；各端的窗口交互、自动发送和复制能力仍以各自源码为准。

- Buffer 的可见性、键入捕获和内容保留相互独立。打开工作台不要求精确输入目标；无目标时仍可在权限与保护条件允许的情况下呈现内容、使用 clipboard-only 模式和显式复制。
- 捕获键入和 IME 投递才要求当前有效的精确目标；保存的 App 名、旧光标矩形或最近客户端不能替代有效 lease。
- 请求和结果必须绑定源修订、workspace/插件身份及 generation；迟到的流式更新、取消结果和旧任务不能覆盖新内容。
- 投递前检查目标身份、组字状态和保护态；如果显式收束组字，随后必须重新取得并复验同一目标。
- 每个块只在平台交付接口接受后消费；失败立即停止，失败块与未发送块保留，不能先清空再尝试插入。
- 接口接受代表一次有效交付尝试，不等于宿主已经保存文档、发送消息或完成远端操作。
- 关闭最后一块后的工作台，需要同一源、generation、session 和目标仍然有效；新内容或任务切换会使旧完成回执失效。
- 可选自动发送同样复验源、块身份与焦点；它不能绕过未结束组字、保护态或用户停用设置。
- 复制生成结果只复制当前已完成且新鲜的结果；复制成功不冒充上屏，也不隐式消费原文。
- 显式卸载、更新、重装、重置或进程重启可以丢弃 Buffer 临时内容；词库、设置与凭据仍须保留。

移动端通过自己的文档/编辑器身份实现相同的过期结果拒绝原则，不直接使用 macOS 的 IMK lease 类型。
共享 [BufferPluginRunner](../../Shared/Sources/RimesCore/BufferPlugin.swift) 串行处理任务，并以请求身份过滤过期 preview/completion。

## 配置三层与方案隔离

下列三层是新增或修改配置时必须遵守的维护契约，具体控件和能力以各端源码为准；字段须说明作用域、默认来源、能力要求、生效方式和修订身份。

1. **方案层**：原生 Rime 资源及用户 `.custom.yaml` 是事实来源。产品只生成自己可追踪的覆盖，不改上游原件或任意覆盖用户手写补丁。
2. **产品偏好层**：Swift、Java、C++ 使用同义的字段和样本；保存值、有效值与已应用修订分别表达，不能以“写入成功”代替引擎生效。
3. **设备层**：字体、像素尺寸、宿主身份、快捷键和权限保留本机适配；设备配置不能覆盖导入方案的编码规则。

不存在强制覆盖所有方案的统一默认。方案默认、用户补丁和产品覆盖发生冲突时，应显示来源并保留未知键。
按方案保存覆盖：内置使用 `builtin:<schema>`，导入使用 `<package-id>:<schema>`；相同 schema 名的不同包不能共享偏好。
缺失字段表示继承；显式 `false` 是用户选择，不能与缺失混淆。恢复默认只撤销本产品拥有的覆盖，不删除用户文件。
能力从实际部署配置和组件读取，不能仅凭文件名含 pinyin/wubi 推断；不可配置、外部管理或探测失败应如实区分。

[InputHabits.swift](../../Shared/Sources/RimesCore/InputHabits.swift) 保存显式覆盖并在写入前读最新快照；[rime_options.hpp](../../Shared/InputHabits/rime_options.hpp) 记录自己改过的选项并恢复原值。
方案切换与选项恢复在引擎队列/锁内、组字安全结束后执行，不能偷交或丢弃未完成组字。
App 与键盘并发写配置时要检测修订，不能让旧快照覆盖新值；移动布局变化也不能在同一触控手势中途改变按键含义。
iOS 的共享配置见 [Settings.swift](../../platforms/ios/Support/Settings.swift)；各端独立存储实现不改变上述隔离与继承语义。

## 方案导入与回滚

导入先只读检查原文件，再在隔离暂存区校验路径、依赖和资源，部署并加载成功后原子发布；启用是独立动作。
失败不得修改当前可用方案、学习词库或设置；移除导入包保留学习数据，内置方案提供回退。
新配置需要部署时，先验证新副本及实际引擎结果，再发布修订；失败恢复旧有效配置，不把半成品宣布为已应用。
ZIP、带编码 TXT/TSV、词库增量及个人短语具有不同语义，不能通过生成独立方案冒充当前方案词库追加。
无编码词表不能静默猜码；模型、原生组件和前端专有 Lua API 必须明确检查，基础方案可用不代表模型能力可用。
实现入口包括 [MacRimeSchemeManager](../../Sources/RimeBuffer/MacRimeSchemeManager.swift)、[iOS RimeSchemeStore](../../platforms/ios/Support/RimeSchemeStore.swift)、[Android RimeSchemeStore](../../platforms/android/app/src/main/java/org/scholay/rimes/android/RimeSchemeStore.java) 和共享 C++ 导入库。

## 插件与独立周边

[OfficialPlugins](../../OfficialPlugins/) 子模块及 [plugins.lock.json](../../plugins.lock.json) 固定已审查源码；[prepare-official-plugins.py](../../scripts/prepare-official-plugins.py) 生成各端导入文件并拒绝覆盖本地改动。
原生插件随宿主编译和签名，下载包只声明宿主支持的配置、提示词和资源，不能授予任意原生执行能力。
提供 Mailbox/Capsule 的前端须保持它们与 Buffer 的独立生命周期；内容从 Mailbox 加入 Buffer 必须是明确操作，不能因后台结果到达就投递前台。
插件停用、卸载、更新或目标失效需要取消工作并撤销相关权限；来源内容、AI 回复和外部工具输入都不能自行扩大交付权限。

## 权限、隐私与持久化

- 普通上屏使用原生输入接口，不通过 Accessibility、模拟粘贴或外部 UI 轮询替代。
- 权限申请由明确操作触发；macOS 捕获使用 [CapturePermissionGuide](../../Sources/RimeBuffer/Capture/CapturePermissionGuide.swift)，不能在普通输入或打开页面时静默索取权限。
- 保护输入、锁屏、会话退出和焦点失效撤销捕获/投递资格；窗口隐藏本身不能被视为旧权限继续有效。
- 用户词库、学习记录、方案偏好、凭据和本地资料独立于程序与构建目录，升级或修复不得以清理为由删除。
- macOS 的 `~/Library/RIMES` 与其他 Rime 前端目录隔离；其他平台同样不得借用或改写其他输入法的数据。
- 导出默认不携带账号凭据、全部输入历史或其他输入法数据；诊断记录不写正文、秘密或完整剪贴板内容。
- 品牌名与兼容身份分开；bundle/package ID、Windows CLSID、偏好键、插件/方案 ID 和已发布资产名按[命名规范](../development/branding.md)保留。
- `CRimeBridge` 与 librime ABI 布局不得重排；持久格式变更需要兼容读取、明确迁移和失败恢复。

## 修改后的验证

按[维护说明](../development/maintenance.md)使用确切提交的远端 CI；静态检查、构建通过、真实宿主确认与公开发行分别记录。
优先覆盖普通输入、组字、候选、失焦/保护、异步过期结果、配置恢复和用户数据保留，不以 UI 截图替代输入链路验收。
