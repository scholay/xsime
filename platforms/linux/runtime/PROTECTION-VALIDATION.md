# Linux 会话和输入保护（任务 9）

对应 [#159](https://github.com/scholay/xsime/issues/159)。以下为源码、隔离 D-Bus 和实际 Fcitx 宿主证据；GNOME/KDE 实机、Firefox 实际窗口和像素比较仍属于 #179，不能用本报告代替。

## 与 macOS 的行为合同

参考 `BufferWindowController.protectForSession` / `restoreAfterSessionProtection`：锁定、休眠、会话失活取并集。保护时保留 Buffer 原文和窗口几何，隐藏窗口，清空预编辑、派生结果及旧目标，取消工作线程和自动发送计时；解锁只恢复原来可见的窗口，不恢复旧输入框或旧任务。显式重新打开或新的输入焦点才能建立新会话。单独的敏感输入框保护保留 Buffer 窗口外框，正文、目标、坐标、结果和复制动作均不可用。

Capsule 保护时收起窗口、清空检索/复制暂存、解除发送目标；协议输出不含卡片标题、预览或正文。持久化笔记及其他用户数据不删除。后续 Mailbox、剪贴板历史等模块应共用该会话信号和授权规则。

## Linux 接口

- 系统总线：logind `PrepareForSleep`，当前会话 `LockedHint` / `Active`、`Lock` / `Unlock` 和 `SessionRemoved`。先按进程查当前会话；必要时使用 `XDG_SESSION_ID` 查询，避免监听其他用户会话。
- 会话总线：freedesktop、GNOME、KDE ScreenSaver 的 `GetActive` / `ActiveChanged`。KDE 服务名为 `org.kde.screensaver`，状态接口仍是 `org.freedesktop.ScreenSaver`，不能调用其只有配置和 AboutToLock 的私有接口。
- Fcitx：Password、Sensitive 任意一位，以及 Disable。全局 FocusIn 和 CapabilityChanged 监听独立于输入法 activate，覆盖密码框关闭 IM 的路径；后台 IC 的普通状态不能解除前台保护。
- 所有 D-Bus 订阅在 Fcitx 事件循环上运行，无外部状态文件或轮询 UI。校验服务所有者和消息签名；各来源独立保存状态和修订，旧查询回复不能覆盖新信号。初始化先隐藏，查询到状态后才解除；曾观察到的服务消失后保持保护，待服务恢复并重新查询。
- 唤醒先重读锁定状态；一个来源的解锁不解除其他来源。真实目标只有通过当前 focused IC 原生 `commitString` 发送。失焦（包括拖动期间）立即撤销派生任务；拖动容错本身不授予旧结果发送权。
- 退出先取消并 join 翻译/AI 线程和 IPC 读线程，再解除事件分派器，防止卸载后的回调访问。显式 Generate 与 debounce 不得替换一个尚在运行的翻译取消句柄。

接口依据：[systemd logind](https://github.com/systemd/systemd/blob/main/man/org.freedesktop.login1.xml)、[KDE ScreenSaver 状态接口](https://github.com/KDE/kscreenlocker/blob/master/dbus/org.freedesktop.ScreenSaver.xml)、[KDE 私有接口](https://github.com/KDE/kscreenlocker/blob/master/dbus/org.kde.screensaver.xml)。

## 验证

2026-10-17，Ubuntu 24.04 arm64 容器 / Fcitx 5.1.7：

- 33 项 CTest：含真实私有 D-Bus 的启动已锁定、多来源叠加、唤醒、GNOME/KDE 接口、服务退出/重启、伪造发送者、错类型信号、旧查询回复；宿主测试验证取消和解锁后的迟到 AI 回调不恢复结果。
- 真实 Fcitx addon：Sensitive、后台 IC 变化、保护中 IPC paste/show/send 拒绝、未结束 Rime 拼音在休眠时清除、锁定/休眠/失活交叠、原文恢复、重新捕获后投递；预先标记为 Password 的新 IC 在不激活 RIMES 时也会屏蔽 Buffer。
- Default Buffer 全链回归、自动发送组合输入/暂停/恢复回归通过。AI HTTP 测试覆盖显式 Shutdown 等待取消回调完成且可重复调用。
- Capsule 协议测试确认敏感输入时不返回上次复制值、检索词和卡片标题；数据库仍保持原内容。

复现：

```sh
cmake --build platforms/linux/ime/build -j4
ctest --test-dir platforms/linux/ime/build --output-on-failure
RIMES_SHARED_DIR=/path/to/staged-rime-data \
RIMES_TEST_SESSION_PROTECTION=1 RIMES_PRIVATE_SESSION_TEST=1 \
dbus-run-session -- platforms/linux/ime/build/rimes-buffer-fcitx-e2e \
  platforms/linux/ime/build . test-data
```

测试服务只在 `dbus-run-session` 隔离总线运行，不向本机真实桌面发送锁屏事件。

## 尚需桌面验收的边界

无 logind 当前会话或无上述 ScreenSaver 服务的桌面不具备完整锁定检测；输入法不能发现应用未报告给 Fcitx 的密码语义。需要 GNOME/KDE 的 X11/Wayland 会话分别验证锁屏、切用户、休眠、Firefox/Chromium 密码框，以及窗口遮罩与 macOS 的截图对照。已连接总线断开时保持保护；总线重建后需要重启输入法重新连接。当前容器结果不代表这些桌面验收完成。
