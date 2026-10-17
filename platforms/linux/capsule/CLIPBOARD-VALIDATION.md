# Capsule 临时剪贴板历史验证（2026-10-18）

对应 #164，依赖 #162、#159。功能已实现并提交验收，问题保持打开，尚未完成像素及真实桌面验收。

## 实现

- 收录默认关闭，设置与模块授权共同控制后台观察。X11 使用独立 XCB / XFixes 连接；Wayland 优先 ext-data-control，兼容 wlr-data-control。事件驱动、有界管道/INCR，传输及 SQLite 操作不阻塞 Fcitx 或 GTK 主循环。
- 原始 MIME/二进制/格式以版本化 CBOR 保存在 0700 目录、0600 SQLite 数据库；拒绝符号链接及不安全数据库/日志文件。全文及来源搜索、分页、去重、收藏、删除、清空和容量淘汰独立于有界 UI 摘要。
- 先检查机密/临时标记，再请求内容。保护、停用及关闭收录撤销在途捕获；恢复和重连仅建立基线。数据库提交前复核当前收录许可；包撤权后旧任务不能继续写入。
- 原始格式恢复携带身份、修订及一次性序列，桌面确认写入后才关闭底栏；快照重连不会重放。有效目标的纯文本经 Fcitx `commitString` 插入；其他内容只显式恢复剪贴板，不发送模拟粘贴。
- GTK 底栏、只读管理详情、图片缩略图、菜单及设置开关已接通。卡片复用时复制读取当前修订。清空采用两次点击确认且保留收藏，保护/隐藏/切换模块立即取消确认。
- macOS 生产模型导出七种类型/投影样例，CTest 检查来源摘要与 Linux 行为。现有原生业务代码未变，仅新增隔离的样例导出命令。

## 已运行

环境：macOS Xcode Swift；Ubuntu 24.04 arm64 Docker，Fcitx 5.1.7、librime 1.10、ICU 74.2、GTK 3.24、SQLite 3、Xvfb、Sway headless。Linux 构建启用 `_FORTIFY_SOURCE=2`。

- Linux 完整构建和 50 项 CTest 通过，含真实 X11 原始多 MIME / 3 MiB INCR、GTK 控制器后台持久化、保护恢复基线、重连不重放、配置/包撤权、修订检查和管理详情只读/隐藏。
- 真实 Fcitx 宿主验证全文中文提交、图片恢复意图与确认时序、收藏/旧修订删除拒绝、全文搜索、键盘筛选/删除和保护态。结构化笔记的 2.7 MiB 分片复制、精确字段插入等既有回归一并执行。
- Sway 真实 wlr-data-control 验证多 MIME、二进制、3 MiB 读取和恢复、保护基线、机密标记和本产品恢复排除。
- UI 被终止后重新启动只保留一个客户端，旧复制序列不会重放。
- macOS debug 构建、clipboard-history 和 clipboard-policy smoke；生产模型导出七个隔离样例。
- 13 项源码安装/卸载生命周期测试通过，包含剪贴板数据库保留哨兵；deb CI 同样检查安装、重装、卸载后保留历史/收藏，并检查新动态库依赖。尚未把此提交的 deb 作为已验证公开产物。

本地日志位于 `.build/linux-parity-20261017/`：`task14-full.log`（50 项 CTest）、`task14-final-host.log`（最终宿主/安装回归）、`task14-wayland-final.log`、`task14-native-build.log`、`task14-native-smoke.log`、`task14-native-history.log`、`task14-respawn-final.log`。

## 未完成的验收

[四主题实际像素对照](../../../validation/linux-capsule-clipboard-20261018/README.md) 为 `requires_review`。字体、图标、顶部捕获入口、多选/存入 Capsule、底栏中文组字与独立搜索、管理/设置页同态截图及完整交互状态仍需对齐；不能把布局尺寸接近记为像素通过。

Sway 覆盖 wlr 协议；ext 协议已实现并编译，但未用提供 ext-data-control 的真实合成器验证。GNOME/KDE 的支持/降级状态、多座席及真实设备仍需在 #179 中检查。Wayland 没有提供应用来源身份的协议字段；界面显示 Wayland，不伪造来源应用。源码、容器、物理桌面和正式 deb Release 验证分别记录。
