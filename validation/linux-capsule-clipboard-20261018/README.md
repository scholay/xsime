# 临时剪贴板底栏实际对照

两端均为生产控件渲染，940×208 逻辑尺寸、2× 输出、五条相同公开样例（四条文本和项目图标）。四主题为 night / day / quiet / rasta。macOS 使用 `clipboard-window-preview`，Linux 使用 `rimes-capsule --clipboard-preview --render`。样例及尺寸来自原生源码提取，源摘要见 `provenance.json`。

[并排及像素差异报告](comparison/index.html) · [差异数据](comparison/manifest.json)。所有结果为 `requires_review`，不表示已像素一致。

已校准：206×126 卡片、8 间距、10 圆角、184×83 图片区域、选中填充/边框、来源标题、22×22 菜单和悬停复制按钮、250×28 搜索壳、管理与关闭按钮。

尚需完成：Inter / SF 与中文字体的字形、字重和换行；应用图标及系统图标；顶部捕获入口（#166）；多选和存入 Capsule 交互；底栏借用 Rime 的中文组字搜索、独立搜索；管理页和设置页的同态双端截图，以及空/错误/悬停/保护等完整状态矩阵。普通卡片内容、来源图标和快捷键按平台实际能力呈现，不能用静态图片伪造交互。

Linux 测试还覆盖原始 MIME 存储、真实 X11 / Sway 传输、Fcitx 提交、保护和管理页操作，见 `platforms/linux/capsule/CLIPBOARD-VALIDATION.md`。真实 GNOME/KDE、ext-data-control 合成器及物理桌面仍需补充验收。
