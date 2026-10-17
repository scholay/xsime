# Linux Mailbox

`rimes-mailbox --show` 打开独立 GTK 工作区。应用菜单的 **XSIME Mailbox**、已启用的 `Ctrl+Shift+M` 和 `rimes-launch mailbox` 复用同一用户目录的窗口。后两者经 Fcitx 快捷键宿主检查；Mailbox 自身监听桌面会话保护。关闭窗口或 `Ctrl+W` 隐藏窗口，`rimes-mailbox --quit` 退出进程。不会结束其他模块的任务或删除会话。

窗口包含终端、对话、收件三个官方模块入口、收件未读角标、列表搜索和筛选。支持查看已有对话/收件、重命名、归档/恢复和本地备注；备注只保存在本机。窗口正常大小为 1000×700，最小 680×500；窄窗口通过顶部侧栏按钮展开列表，点击正文外侧或 Escape 收起。显示并激活的收件才标记已读。窗口尺寸与最后模块自动保存；未发送草稿按模块/会话保留在当前进程，退出进程不持久化草稿，与当前 macOS 行为一致。

本次是 [第 18 项宿主任务](https://github.com/scholay/xsime/issues/168)。收件附件的查看/复制和 Capsule/Buffer 流转在 #169，来源接入在 #170，CLI 在 #171/#172，AI 续聊在 #173，PTY 终端在 #174。当前终端页说明运行组件未就绪，AI 对话尚不能发送请求；导航声明不代表这些后续能力已完成。

## 安装、更新与保留数据

Debian/Ubuntu 推荐从项目同一个 `v*` Release 安装完整版 deb；开发源码安装/卸载方法见 [IME 安装说明](../ime/README.md)。deb 和 `install.sh` 同时安装 `rimes-mailbox` 与桌面菜单入口。卸载 deb 使用 `apt remove fcitx5-rimes`；源码安装使用相同 prefix 的 `uninstall.sh`。不要直接删除共享安装目录。

用户根目录为 `RIMES_USER_DIR` 或 `$XDG_DATA_HOME/rimes`（默认 `~/.local/share/rimes`），其下 `mailbox/` 包含：

- `mailbox.json`：兼容 macOS schema-v1 的会话、消息、来源、处理状态与 Foundation 日期。
- `images/<UUID>.png`：发布消息前先独立持久化的 PNG 附件。
- `window.json`：窗口大小和最后模块；`host/`、`window/` 中的锁文件只用于进程与写入协调。

这些目录为 0700、文件为 0600。内容写入使用原子替换，写操作校验预期修订、模块授权与会话状态。关闭、停用模块、重装和卸载保留 Mailbox 数据，也保留用户词库和学习记录。数据损坏时显示错误并保留原文件，不以空文档覆盖；恢复备份前先退出 Mailbox。跨总线的第二个进程也不能夺取同一目录的主实例锁。

## macOS 组件同步

macOS 的生产 UI、模型与官方模块声明是基准。`MailboxUI` 共享命名尺寸常量，`mailbox-linux-fixtures` 用隔离目录/偏好导出真实 AppKit 截图、解析/筛选样例和主题色。Linux 通过 `verify_native_contract.py` 检查来源摘要，并消费相同尺寸、模块和颜色；修改基准后必须重新生成并审查，构建不会静默使用旧基准。

```sh
# macOS，仓库根目录，已完成隔离 debug 编译
TZ=Asia/Tokyo .build/out/Products/Debug/RimeBuffer mailbox-linux-fixtures /tmp/mailbox-native
python3 platforms/linux/mailbox/verify_native_contract.py --import-fixture /tmp/mailbox-native/native-mailbox.json
# Linux，使用真实 GTK 工作区导出同尺寸 2× 截图
TZ=Asia/Tokyo xvfb-run -a platforms/linux/ime/build/rimes-mailbox \
  --render-fixture platforms/linux/mailbox/tests/fixtures/native-mailbox.json \
  --render-dir /tmp/mailbox-linux
```

[验证记录](HOST-VALIDATION.md) 和 [双端实际截图](../../../validation/linux-mailbox-host-20261018/README.md) 分开记录行为与视觉。当前截图仍需审查；功能测试通过不代表像素一致或真实 GNOME/KDE 桌面已验收。
