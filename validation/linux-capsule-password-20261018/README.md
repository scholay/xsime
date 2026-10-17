# 密码库管理页实际对照

生产管理页、同一公开标题/摘要/登录分类、940×660 逻辑尺寸及 2× 输出，覆盖 night / day / quiet / rasta。macOS 使用隔离密码库及偏好的 `capsule-password-visual-fixtures` 导出，Linux 使用 `--password-preview --render` 导出。PNG 不包含个人数据或解密后的正文。

[并排及像素差异](comparison/index.html) · [差异数据](comparison/manifest.json) · [来源与摘要](provenance.json)。比较先将嵌入 ICC 转为 sRGB，不缩放原始截图；全部结果保持 `requires_review`。

本次调整了字段顺序、四个等宽口令槽（23 px）、槽间距（8 px）、30 px 输入区、主题背景与底部操作位置。仍存在字体度量、原生输入框/下拉框、图标、列表选中态、底部口令设置入口及按钮状态差异。macOS 是无法获得 key window 时的实际离屏渲染，Linux 是 Xvfb 实际 GTK 渲染，激活态不能视为同条件通过。新建、解锁、编辑、错误、悬停及底栏内联详情需继续补齐。

macOS key-window 交互烟测在当前宿主无法使隔离窗口成为 key window，已记录失败而未绕过断言；本报告仅是生产布局的渲染证据。Linux 的实际聚焦及安全交互另由 Xfwm4 + GTK 测试验证。此对照不能用于关闭 #167 或 #179。
