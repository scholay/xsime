# Mailbox 收件双端实际对照

同一份公开消息、处理状态和 PNG，真实 macOS AppKit 与 Linux GTK 生产组件各导出 48 张 2× 截图：inbox/chat/terminal/image/accepted/rejected × night/day/quiet/rasta × 1000×700 或 680×500，时区均为 Asia/Tokyo。

[并排及差异图](comparison/index.html) · [差异数据](comparison/manifest.json) · [来源与摘要](provenance.json) · [行为验证](../../platforms/linux/mailbox/INBOX-VALIDATION.md)。ICC 转为 sRGB 后逐像素比较，不缩放原图。全部仍为 `requires_review`。

本次补齐了复制、存入 Capsule、加入 Buffer、保存图片和忽略操作，并以原生尺寸校准操作栏、正文段落、备注背景和 PNG 缩放。未解决的视觉差异包括字体度量、图标、搜索/分段/发送控件，以及删除和返回终端的工具栏入口。后续 AI/Markdown 与 PTY 功能、悬停/焦点/禁用/错误/保护状态，继续随对应任务及 #179 对照；当前报告不宣称像素通过。

`linux-roundtrip` 是由测试修改的公开数据，由原生 MailboxStore 再读验证所有 11 个会话及 PNG 摘要；它与视觉样例分别留存来源，不含个人消息、凭据或远程 AI 请求。AppKit 离屏和 Xvfb 渲染不代替实际 GNOME/KDE/Wayland 桌面验收。
