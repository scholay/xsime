# Capsule 结构化笔记验证（2026-10-18）

对应 #163，依赖 #162。实现与验收中，视觉差异未关闭。

## 实现

- 以生产 Swift 代码生成语法、front matter、摘要、Bullet、Unicode 字素和 AppKit RTFD 样例，Linux CTest 检查源码摘要及相同行为。包含中文、组合字符、emoji、Unicode 序号、代码块和未知元数据。
- 富文本编辑保留样式、链接、图片字节与尺寸；有界 flat-RTFD 读取不解压到文件系统。Linux 导出由 AppKit 回读文字、Markdown 和附件原始字节。无法无损编辑的格式保留原始资产并只读展示正文。
- SHA-256 修订比较、外部 Markdown 投影冲突确认、单条损坏降级。inotify 观察外部原子替换，脏草稿不被后台刷新覆盖；授权撤销或保护态会取消模态编辑、隐藏正文并保留普通未保存草稿。
- 底栏详情保留分类栏、左侧信息和右侧分组行；第一行默认选中，逐行复制值/原生插入与整条插入分开。旧修订动作先重新读取验证，不能投递过时值。
- Capsule 快照上限 16 MiB，非阻塞有界写入、最新快照合并；保护态撤销遇到半帧即断开并重新同步，重连不重放复制。Buffer 的帧大小保持独立。

## 已运行

环境：本机 macOS Xcode Swift 工具链，Ubuntu 24.04 arm64 Docker，Fcitx 5.1.7、librime 1.10、ICU 74.2、GTK 3.24、Xvfb。未安装到本机正在使用的输入法。

- 完整 Linux 构建与 46 项 CTest 通过；后续细节修复重新运行 11 项 Capsule 回归。
- 真实 GTK 管理页：富文本/图片保存、撤销重做、Bullet、草稿保护恢复、显式 Markdown 导入、并发修订拒绝；图片实际解码为 pixbuf，不能以占位标签冒充渲染成功。
- 真实 Fcitx：详情按键与 IPC、精确值 commitString、旧修订拒绝、密码输入隐藏、2.7 MiB Unicode 复制分片、Buffer 优先；UI 杀掉重启不重放复制。
- macOS 完整 debug 构建；plugin-distribution、capsule-structure、capsule-window、capsule-rail、capsule-interaction 与引擎 smoke 通过。
- 本地日志：`.build/linux-parity-20261017/task13-full.log`、`task13-post-review.log`、`task13-image-final.log`；原生日志在 `task13-native/`。

## 视觉与平台边界

[实际详情对照](../../../validation/linux-capsule-notes-20261018/README.md) 保留 macOS / Linux PNG、并排及差异图。同一生产样例、940×208 逻辑尺寸、2× 输出，状态为 `requires_review`。已校准分类栏、206 宽信息面板、24 高数据行、18 高标题、选中底色和默认选择；仍有字体、图标、顶部搜索/操作区等差异，不能宣称像素一致。

管理页富文本截图为 Linux 实际控件；对应原生富文本管理页及完整主题/状态矩阵仍需在 #179 补齐。真实 GNOME/KDE、多屏及输入框验收也未完成。大文本复制测试覆盖 Fcitx→GTK IPC；实际 Wayland 剪贴板传输和历史收录由 #164 接续验证。
