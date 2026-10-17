# 捕获历史卡片实际对照

macOS 原生生产 CaptureHistoryCard 与 Linux 实际底栏卡片，206×126 逻辑尺寸、2× 输出、night/day/quiet/rasta 四主题。两端使用同一个 CaptureSmoke 生成的公开图案；包含截图、导入视频、时长、收藏、未完成及 emoji 标题，没有个人捕获内容。

[并排与差异报告](comparison/index.html) · [差异数据](comparison/manifest.json) · [源摘要](provenance.json)

卡片尺寸、缩略图填充裁剪、圆角、标题字符边界及两行文字契约已对齐。已修正菜单按钮背景/圆角、详情颜色和高分辨率缩略图；Linux 推荐 Noto emoji 字体，缺少字体时仍由系统回退。

所有结果保持 `requires_review`，字体度量/字重、SF Symbols 与 Cairo 符号、系统 emoji、抗锯齿仍有差异。完整取景工具栏、管理/编辑页、悬停/错误/保护及更多窗口/缩放状态尚未全部对照，不能据此关闭 #166 或 #179。

复现：macOS `RimeBuffer capture-linux-fixtures <native目录>`；Linux `rimes-capsule --capture-preview <native/fixture.png> --capture-kind image|video --theme <主题> --render <文件.png>`。比较脚本为 `platforms/linux/runtime/compare_visuals.py`，统一嵌入 ICC 到 sRGB 后输出。功能与安装边界见 [捕获验证](../../platforms/linux/capsule/CAPTURE-VALIDATION.md)。
