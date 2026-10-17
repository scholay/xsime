# 资源与文献管理页实际对照

两端生产管理页均为 940×660 逻辑尺寸、2× 输出，night / day / quiet / rasta 四主题。使用同一不存在的可移植 PDF 路径及公开文献样例，不含个人数据。macOS 通过 `capsule-resources-preview`，Linux 通过 `rimes-capsule --resources-preview --render` 生成；原生引用样例另由生产 formatter 导出并用于 Linux 28 项比对。

[并排及像素差异报告](comparison/index.html) · [差异数据](comparison/manifest.json) · [源摘要](provenance.json)。所有结果为 `requires_review`，尚未达到像素验收。

已对齐管理页尺寸、三栏位置、PDF 元数据顺序、路径行的选择及复制入口、主题颜色和六种单条引用格式。Linux 的显式附件副本选项置于表单底部，避免挤占原生字段顺序。

剩余差异包括字体度量/字重、图标、控件圆角与滚动条、当前列表选中态；其他资源类型、窗口尺寸、滚动位置及错误/保护/悬停状态仍需补齐。Ctrl 与 ⌘ 快捷键按平台表达。当前对照用于继续校准，不能据此关闭 #165 或 #179。

功能、宿主与安装验证见 [资源验证记录](../../platforms/linux/capsule/RESOURCES-VALIDATION.md)。
