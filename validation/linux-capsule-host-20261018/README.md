# Capsule 管理页实际渲染对照

2026-10-18，任务 #162 / #179，**尚未通过像素验收**。

[打开并排与差异报告](compare/index.html)。`macos/` 和 `linux/` 保留原始 PNG，`compare/manifest.json` 记录转换到 sRGB 后的差异度量；差异图仅放大差异的亮度，不修改原图。

macOS 使用隔离的 XSIMEParityPreview.app 的生产 `capsule-manager-preview` 入口，分别设置 `RIMEBUFFER_APPEARANCE_MODE=night/day/quiet/rasta`，用户目录、插件目录和 UserDefaults 域均隔离。Linux 使用生产 `rimes-capsule --manager-preview --theme THEME --render PATH`，GTK 3/Xvfb。两端均为 940×660 逻辑尺寸、1880×1320 输出。三条样例从 macOS `CapsuleRailSmoke.swift` 构建时提取，加上相同的默认笔记，不含私人内容。

生成报告：

```sh
python3 platforms/linux/runtime/compare_visuals.py \
  --macos validation/linux-capsule-host-20261018/macos \
  --linux validation/linux-capsule-host-20261018/linux \
  --output validation/linux-capsule-host-20261018/compare
```

需要 Pillow。CI 将继续上传 Linux 四主题渲染，便于发现后续漂移。

已对应：窗口尺寸、标题栏、三栏区域、列表宽度、主背景/表面语义色、笔记表单及固定样例。待修正：字体度量、部分表单间距、侧栏/内联按钮样式、选中与焦点状态、图标轮廓。当前 macOS smoke 未激活窗口，Linux 渲染清除了文本焦点；活跃/不活跃的控件状态仍需严格配对。请勿把本报告作为像素一致或物理桌面验收完成的证据。
