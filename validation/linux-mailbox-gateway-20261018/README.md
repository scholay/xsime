# 本地网关设置双端对照

真实 AppKit / GTK 设置页各 16 张 2× 截图：Cursor、Codex、Claude Code、通用 MCP × 启用/关闭 × 墨竹/翡翠，逻辑尺寸 980×680。内容均为脱敏配置，未读取个人令牌。

[墨竹并排与差异](comparison/night/index.html) · [翡翠并排与差异](comparison/day/index.html) · [来源摘要](provenance.json) · [功能验证](../../platforms/linux/mailbox/GATEWAY-VALIDATION.md)。比较前将内嵌 ICC 转为 sRGB，保留原始像素，不缩放后冒充一致。

已按原生页面校准 650 宽设置行、626 宽配置、客户端分段均分、图标底板、配置框背景、文本层次、禁用和补充命令状态。字体度量/字重、SF Symbols 替代图标、控件边框与垂直位置仍有差异；所有报告为 `requires_review`。更多主题和桌面交互继续归入 #179，当前截图不代表像素通过。

`linux-roundtrip` 为真实 TCP 测试写入的两条公开消息；macOS 原生 MailboxStore 验证来源、正文、日期与审核状态一致。Linux 自有去重元数据保留在实际文档中，原生 Codable 按兼容规则忽略该扩展。该样例不含个人收件、真实凭据或外部模型请求。
