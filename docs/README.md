# XSIME 文档

这里集中维护仍有用途的使用说明、架构契约和开发规范。旧构建验收流水、临时审计、分支整合记录与已完成的发布准备不再保留独立文档，需要追溯时查看 Git 历史、对应 PR 或正式 Release。

## 从这里开始

| 需要做什么 | 入口 |
| --- | --- |
| 安装与了解产品 | [项目 README](../README.md) |
| 接手开发 | [系统架构](architecture/system.md) · [维护规则](development/maintenance.md) |
| 导入方案或修复词库 | [方案导入](portable-scheme-import.md) · [用户词库恢复](user-dictionary-recovery.md) · [五笔输入](wubi.md) |
| 发布与更新 | [发布流程](release/README.md) · [实现参考](release/reference.md) |
| 查看已发布变化 | [更新日志](../CHANGELOG.md) · [1.2.0 发布记录](releases/1.2.0.md) |
| 了解后续产品方向 | [产品路线图](plans/roadmap.md)，规划与已实现能力分别判断 |

## 五个平台

| 平台 | 工程入口 | 专项说明 |
| --- | --- | --- |
| macOS | [Sources 与构建入口](../Package.swift) | [系统架构](architecture/system.md) · [输入源缓存恢复](macos/input-source-cache-recovery.md) |
| iOS | [平台 README](../platforms/ios/README.md) | [发布](platforms/ios/release.md) · [隐私](platforms/ios/privacy.md) · [资源来源](platforms/ios/resources.md) |
| Android | [平台 README](../platforms/android/README.md) | [发布与验证](platforms/android/release.md) |
| Windows | [原生 TSF 工程](../platforms/windows/native/README.md) | [连接器](platforms/windows/connectors.md) · [外围功能](platforms/windows/peripherals.md) · [验收清单](platforms/windows/manual-testing.md) |
| Linux | [Fcitx5 工程](../platforms/linux/ime/README.md) | [Buffer](platforms/linux/buffer-spec.md) · [Capsule](platforms/linux/capsule-spec.md) · [验收清单](platforms/linux/acceptance.md) · [包生命周期](platforms/linux/release-lifecycle.md) |

## 功能与开发约定

- 输入：[候选矩阵](features/candidate-matrix.md)、[并击键位](features/chord-keymaps.md)、[统计与跟打](features/typing-metrics.md)。
- 内容：[Capsule](features/capsule.md)、[捕获](features/capture.md)、[Mailbox](features/mailbox-workspaces.md)。
- 插件：[配置契约](development/plugin-configuration.md)、[电音演奏](features/buffer-music.md)、[摩斯电码](features/morse-code.md)、[学术工具](features/scholay-buffer.md)。
- 工程：[命名与兼容](development/branding.md)、[贡献规范](../CONTRIBUTING.md)、[验收附件规则](../validation/README.md)。
- 分发：[未签名预览](release/unsigned-preview.md)、[跨平台数据预览](release/platform-preview.md)。

法律与来源说明保留在根目录：[许可范围](../LICENSING.md)、[第三方声明](../THIRD_PARTY_NOTICES.md)、[来源与署名](../ATTRIBUTION.md)。源码目录中的 README 只服务相邻工程、资源或工具；构建实际依赖的许可证、商店材料和工具技能保留原路径。

## 文档维护规则

新增说明优先补到现有专题，避免为每次实现、构建或问题排查新建 Markdown。只有会持续维护的独立主题才新增文件，并在本页添加入口。

功能与配置以当前源码及明确发布记录为依据；不把旧版本的测试结果、提交或审核状态写成当前事实。操作说明集中维护一份，平台 README 通过链接引用。截图与原始日志不进入 Git；结构化摘要不能冒充真实宿主验收。

`docs/index.html` 与 `docs/ios/` 是既有公开网站页面，和本工程文档入口用途不同，不因文档整理更改其 URL。
