# Capsule 本机内容库

Capsule 管理临时剪贴板内容与用户明确保存的资料。它与 Buffer、Mailbox 并列，拥有独立窗口、存储和生命周期。本文说明 macOS 实现；其他桌面端见 [Windows 外围功能](../platforms/windows/peripherals.md)与 [Linux Capsule](../../platforms/linux/capsule/README.md)。

## 使用与投递

- 底栏展示临时内容和已保存条目，可搜索、选择、复制或按当前支持的方式放入目标应用。
- 齿轮打开管理界面；保存、编辑、迁移和删除由用户明确操作。
- 截图、录屏和编辑见[捕获说明](capture.md)。快捷键以设置中实际保存的绑定为准，升级保留用户自定义值。

管理窗口取得正常键盘焦点。复制、剪贴板粘贴和 IME 文本投递是不同路径；不能借管理窗口保存的旧焦点直接向宿主写入。没有所需权限时采用已有降级行为，不从普通打字路径弹出权限申请。

## 数据边界

[内容模型](../../Sources/RimeBuffer/CapsuleContentStore.swift)保留 `Password`、`Skill`、`Note`、`Image`、`PDF` 五个稳定类型。界面分组与条目类型分开，展示名称变化不能改写持久化标识。

macOS 数据位于 `~/Library/RIMES/`：

| 路径 | 用途 |
| --- | --- |
| `capsule/entries/` | 普通条目的 Markdown 与 front matter |
| `capsule/passwords/` | 加密密码记录 |
| `capsule/assets/` | 按内容摘要命名的本机媒体 |
| `capsule/conflicts/` | 并发编辑产生的冲突副本 |
| `capsule/master-key` | 仅本机保存的加密密钥 |
| `capsule-sync/` | 用户选择的同步目录配置与修订状态 |

写入经存储层的身份、修订、私有权限和原子替换检查。笔记保存保留产品不拥有的 front matter；无法解析的条目提供修复入口，不因一条损坏记录清空整个库。图片/PDF 预览按需生成并限制资源使用，异步结果必须重验条目身份和修订。

## 密码与保护状态

[密码存储](../../Sources/RimeBuffer/CapsulePasswordStore.swift)将机密字段加密保存。查看、复制和自动遮蔽由独立授权状态控制；焦点、条目、口令、会话或保护状态变化会撤销旧授权。

密码复制使用敏感剪贴板标记，不进入临时历史，也不会自动投递。笔记迁移为密码先完成加密写入，再按原修订处理来源，不能覆盖并发修改。密钥、密码与口令不进入普通日志、仓库或同步镜像。

## 可选 iCloud 同步

本机目录始终是权威副本。用户显式选择同步目录后，[同步控制器](../../Sources/RimeBuffer/CapsuleCloudSync.swift)逐条处理可同步内容、媒体摘要、修订、删除标记和冲突，不移动或直接镜像整个用户目录。

密码、主密钥与只在当前设备有效的路径不作为可移植内容同步。关闭同步停止后续自动任务，不删除两端数据。缺失、损坏或冲突的远端内容不能直接覆盖本机有效记录。

## 维护入口

- [管理窗口](../../Sources/RimeBuffer/CapsuleWindowController.swift)：内容操作与独立窗口生命周期。
- [底栏](../../Sources/RimeBuffer/ClipboardHistoryWindowController.swift)：临时历史、条目选择和激活。
- [维护与验收规范](../development/maintenance.md)：远端 CI、实际宿主验收和数据保留要求。

卸载、更新和开发清理不能顺带删除用户内容库。验收截图与原始日志按[附件规则](../../validation/README.md)存放。
