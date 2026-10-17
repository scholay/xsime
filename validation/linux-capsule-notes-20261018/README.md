# Capsule 笔记详情实际对照

2026-10-18，#163 / #179，**尚未通过像素验收**。

[并排及差异图](compare/index.html)：左侧 macOS AppKit，右侧 Linux GTK。原图各自保存在 `macos/` / `linux/`；统一 ICC→sRGB 后的指标在 `compare/manifest.json`。相同生产 `CapsuleInteractionSmoke.swift` 样例、夜间主题、940×208 逻辑尺寸、2× 输出，第二行选中并显示复制反馈。不包含个人内容。

macOS：隔离用户目录的 `RimeBuffer capsule-interaction-smoke OUTPUT_DIR`。Linux：`rimes-capsule --detail-preview --theme night --render PATH`，实际 GTK/Cairo 渲染。构建时从原生源码提取标题、正文、摘要与字段，源文件哈希写入生成合同。

已对齐：分类栏继续显示、左侧信息宽度、详情行/分组高度、字段标签位置、返回位置与选择色。仍有字体字形与基线、图标轮廓、顶部搜索及操作入口、外框圆角等差异；完整焦点/悬停/保护态和其他主题仍需验收。不能用本报告宣称像素一致。

`linux-rich-manager.png` 是 GTK 管理页实际富文本编辑截图，包含可解码图片资产；当前没有同状态原生管理页配对图，不纳入像素通过判断。完整测试见 [笔记验证记录](../../platforms/linux/capsule/NOTES-VALIDATION.md)。
