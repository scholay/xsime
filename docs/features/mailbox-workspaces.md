# Mailbox 工作区

独立 Mailbox 窗口使用原生 AppKit 插件宿主。全局 Mailbox 快捷键和通知打开同一个窗口，默认内容尺寸 1000 × 700，最小内容尺寸 680 × 500，支持浅色和深色外观，并保留用户调整的窗口尺寸。

导航栏宽 64pt，图标、名称和未读角标各自定位；正文、标题和输入区使用一致的 16pt 边距，操作按钮统一为 28pt 高。窄窗口自动收起列表和终端扩展区，可用工具栏按钮展开抽屉；选择会话或点击空白遮罩后收起。展开抽屉不挤压正文、不调整终端列数，窗口加宽后自动恢复分栏。也可以手动收起宽窗口的侧栏。

## 三个内置插件

| 插件 | 注册键 | 行为 |
| --- | --- | --- |
| 终端 | `builtin.mailbox.terminal` | SwiftTerm PTY，直接启动 Codex、Claude Code 或 `/bin/zsh -l`；每个会话独立选择工作目录。右侧支持文件预览、手动收取和会话状态。 |
| 对话 | `builtin.mailbox.chat` | 使用现有 AI 连接器、冻结的 provider/model 路由和会话存储；支持新建、搜索、重命名、归档、流式显示与草稿。 |
| 收件 | `builtin.mailbox.inbox` | 展示单向推送、图片和手动收取的终端产物；支持未读/待处理/归档筛选、图片保存、Capsule 保存和本地备注。 |

插件管理增加 Mailbox 分类。停用隐藏工作区，保留内容及已经启动的终端。导航切换和关闭窗口不结束 PTY；“结束”才终止选中的进程。终端会话在本次 RIMES 运行期间保留，重启后不会重建进程；已收取产物仍可读取。

## 内容与权限

- 原有 schema-v1 Mailbox 数据保持兼容。`workspace`、`terminalSessionID`、`archivedAt` 为可选字段；旧内容按来源、图片附件和既有图片任务标题路由。
- 终端不伪装成聊天消息。收取文件是用户明确操作，以进程内 UUID 关联原会话，不根据工作目录、终端标题或相似文件名推测归属。
- 图片转存 Mailbox 私有目录后再原子发布收件；失败删除该次新附件。存入 Capsule 的图片另行持久化，不依赖 Mailbox 附件以后仍存在。
- 文件预览只处理有界的普通图片和 UTF-8 文本；目录列表不递归，最多展示 60 项。此版本不自动判断 CLI 任务完成，也不自动扫描、上传或收取整个目录。
- 收件输入框只添加本地备注，不调用发送接口。外部消息加入 Buffer 仍经过现有 Inbound review；没有绕过焦点、stale 或插件来源检查。AI 结果与用户明确选择的本地成果只做 Buffer 暂存，插入仍由既有 Delivery 路径负责。
- 流式预览继续只存在于内存，原位更新一个正文视图；完成后才持久化最终回复。对话的 provider/model 不因全局设置改变而被静默替换。

## 验证

`mailbox-workspace-smoke [output-directory]` 使用临时存储和真实 Shell PTY，不调用在线 AI、不写入用户 Mailbox。覆盖旧数据路由、筛选、归档持久化、图片收件原子性、草稿、流式行身份、三个工作区的最小窗口尺寸、抽屉开关、长标题/按钮边界、深浅色渲染、工作区切换/关窗后进程保留及终端输入/输出/退出。

相关回归：`mailbox-store-smoke`、`mailbox-window-smoke`、`ai-text-mailbox-smoke`、`mailbox-toast-smoke`、`plugin-platform-smoke`、`settings-routing-smoke`、`codex-session-smoke`。原生截图可输出至 `.build/mailbox-workspace-qa/`；截图中的内容是隔离测试数据。

终端的 `cacheDisplay` 离屏导出会丢失 SwiftTerm 的图层背景，并可能继承错误的文字变换；不能用该导出判断终端文字是否对齐。`RIMES_MAILBOX_LAYOUT_REVIEW=1` 会在 680pt 的真实终端测试窗口停留 45 秒，便于检查屏幕上的布局。此次已在真实窗口核对终端输出、扩展区展开和收起；PTY 测试另验证输出内容及缩放后的保留。
