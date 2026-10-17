# Capsule 密码库验证（2026-10-18）

对应 #167。加密存储、查看验证、敏感复制和结构化 TOTP 已实现；GNOME 剪贴板、底栏内联详情、实际桌面与像素对齐尚未完成，任务保持打开。

## 格式与授权

- 与 macOS 共享密码文档 v1/v2、32 字节主密钥、12 字节 nonce + ciphertext + 16 字节 tag，以及 `capsule-password-v1\n<UUID>\n<title>` 认证元数据。使用 OpenSSL EVP ChaCha20-Poly1305；旧字段转为相同 Markdown，双格式同时存在时也采用原生的有效旧记录优先顺序。
- 目录 0700、文件 0600；拒绝符号链接、不安全主密钥、过大文档、身份错配及修订冲突。现有密码库缺少密钥时停止，不生成新密钥覆盖。所有保存、删除及凭据修改在最终写入前检查取消/身份/权限；文件锁争用快速失败，不让外部占锁阻止明文隐藏。
- 查看口令摘要与 macOS 偏好中的版本、salt 和规范化物理键顺序兼容。Linux 单独存为私有文件；文档迁移不自动迁移平台偏好。四组默认/自定义键、旧口令验证、二次确认、恢复默认及凭据替换后的失效均测试。该口令是界面授权，不是派生加密密钥。
- 密码主体只在 GTK 组件内解密；Fcitx IPC 只含公开标题、摘要、类别和修订。标题搜索不读取正文。普通 copy、row-insert 和输入法提交不能旁路查看验证。
- 60 秒详情、15 秒字段和编辑器租期使用单调时钟。失焦、隐藏、模块/记录切换及保护态立即清除；外部文档、口令和插件变化由后台检查撤销。正文不使用外部输入上下文；PRIMARY、拖出及默认复制被禁用，迟到粘贴须再次检查当前会话。
- 敏感复制在实际取得所有权时重新检查插件授权、凭据、记录修订和租期；数据附带四类机密/临时标记。本应用历史忽略这些数据；仅释放自己的 X11 owner window 或 Wayland source，避免 60 秒到期时清除后来复制的内容。
- 笔记迁移先加密再按原修订删除原文，冲突时保留原文并撤销新记录；不继承公开摘要，不清理既有备份。卸载不删除主密钥、查看凭据或文档。

## 已验证

Ubuntu 24.04 arm64、GTK 3.24、Fcitx 5.1.7、OpenSSL 3；Xvfb + Xfwm4 与实际 headless Sway。macOS 仅隔离 debug 构建及公开样例，没有安装/重启当前输入法。

- macOS 生产存储生成 6 组 v1/v2/双格式样例，Linux 解密、编辑、重新加密，macOS 再逐项读取验证；30 个原生 TOTP 输出和 RFC 6238 的 6 个 SHA-1 标准向量通过。样例含公开随机测试密钥，禁止用真实密码更新样例。
- 存储：四个并发 CAS 写入仅一个成功、篡改标题/密文、损坏记录隔离、丢失主密钥、权限和符号链接、备份恢复、摘要敏感字段拒绝、迁移冲突，以及已取消写入/删除/凭据修改。
- 真实 GTK：四组物理键、错误与修饰键、脱敏行、TOTP 种子隐藏、字段复制及编辑器 Ctrl+C、PRIMARY 排除、加密新建/编辑/删除、真实窗口失焦、15 秒编辑到期、外部修订/凭据/插件撤权、保护隐藏，以及外部占锁时立即遮蔽。
- 真实 X11 和 Wayland：粘贴读取、机密标记、模拟单调时钟 59/61 秒边界、连续四轮后来复制内容保留、保护清理及普通复制保留。原有多 MIME、大数据、图片、文件和历史排除测试同时通过。
- 真实 Fcitx：公开卡片、仅标题搜索、正文不可搜索、普通复制/行插入拒绝、Return 打开验证、保护态移除卡片及正文不进入返回快照；笔记、资源和捕获回归同时通过。
- 完整 Release / 锁定 Rime 的全新 deb 构建通过全部 62 项 CTest（64.40 秒）；17 项源码安装/卸载与版本测试通过。安装保留哨兵增加主密钥、查看凭据、加密文档。
- 实际安装、重装并卸载 `fcitx5-rimes_1.1.5_arm64.deb` 通过；动态依赖完整，词库、学习、笔记、附件、剪贴板、捕获、主密钥、查看凭据和加密文档的全部哨兵哈希不变。

日志为 `.build/linux-parity-20261017/task17-*.log`。测试与公开样例入口在 `capsule/tests`；CI 增加私有窗口管理器依赖和四主题密码页渲染。

## 待验收

[四主题实际对照](../../../validation/linux-capsule-password-20261018/README.md) 全部为 `requires_review`。已调整口令槽、输入区与操作区布局；原生控件、字体/图标、选中态、激活态及解锁/新建/错误状态仍需对齐。macOS key-window 交互烟测本机失败，离屏渲染不代替交互通过。

Linux 目前从底栏打开管理窗口，尚未实现原生底栏内联解锁详情。Wayland 敏感复制需要 ext/wlr-data-control；GNOME 等桌面的受焦点剪贴板路径待实现。实际 GNOME/KDE/wlroots 的锁屏、休眠、多屏与缩放继续由 #167/#179 验收。未发布正式 v* 标签或公开安装包。

参考：[RFC 6238](https://www.rfc-editor.org/rfc/rfc6238.html)、[OpenSSL EVP ChaCha20](https://docs.openssl.org/3.0/man3/EVP_chacha20/)、[GTK 输入上下文](https://raw.githubusercontent.com/GNOME/gtk/gtk-3-24/gtk/gtkimmodule.c) 与 [GTK PRIMARY 生命周期](https://docs.gtk.org/gtk3/method.TextBuffer.remove_selection_clipboard.html)。

## PR review regression checks (2026-10-18)

The category popup now defers its focus decision until GTK has processed focus events, and only accepts a menu attached to that category control. The real X11/GTK test opens the dropdown, selects a category, verifies encrypted save, then moves focus outside an open dropdown and verifies immediate conceal. The existing 15-second, credential, plugin, record, and lock revocation cases remain in the same test.

Note migration rechecks cancellation and the credential under the source deletion lock; cancelling after encrypted publication retains the original note and rolls back the new vault record. Buffer prefix consumption rejects a boundary inside a UTF-8 sequence before changing state. Regression tests cover both CJK and emoji.

The IPC snapshot now distinguishes desktop `session_protected` from the aggregate protected-input flag. Entering a password field still conceals the vault and cancels pending writes, but preserves an already published explicit copy for its existing lease. Desktop lock/sleep and disabled vault modules still revoke the sensitive clipboard owner; legacy snapshots without the new field fail closed. Protocol round trips, real GTK/X11 clipboard paste after password-field protection, clipboard withdrawal on session lock, and the actual Fcitx password-capability snapshot pass (`task17-copy-*.log`).
