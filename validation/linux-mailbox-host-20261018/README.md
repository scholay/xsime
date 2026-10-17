# Mailbox 双端实际对照

macOS AppKit 与 Linux GTK 的生产工作区，使用同一公开会话/消息、Asia/Tokyo 时区、四种主题和两种内容区域尺寸：1000×700、680×500，均为 2× PNG。覆盖终端、对话、收件，共 24 对；不包含个人数据或远程 AI 请求。

[并排及差异图](comparison/index.html) · [差异数据](comparison/manifest.json) · [来源与摘要](provenance.json) · [行为验证](../../platforms/linux/mailbox/HOST-VALIDATION.md)。比较先将嵌入的 ICC 转为 sRGB，不缩放原图，不自动判定像素通过。当前全部为 `requires_review`。

已对照并调整导航/侧栏宽度、最小窗口与收起行为、标题和行高、间距、主题背景、未读角标与输入区圆角。仍需处理：SF/PingFang 与 Inter/Noto 的字体及字重差异、图标形状、搜索框/分段按钮、正文段落间距，以及 #169 收件操作、#173 Markdown/AI 对话、#174 PTY 内容。悬停、聚焦、错误、锁屏与禁用状态的完整截图矩阵继续由 #179 汇总验收。

macOS 截图由隔离数据和偏好的真实生产视图离屏导出，Linux 使用 Xvfb 的真实 GTK 视图。两端都截取应用内容区域，不包含操作系统标题栏。它们不代替物理桌面交互验收。本目录中的 `linux-roundtrip` 是公开的五会话/一图片格式互读样例，与视觉样例各自保留来源；不是用户资料的备份。
