# iOS 构建与发布

版本及工程配置来自 [`platforms/ios/project.yml`](../../../platforms/ios/project.yml)，Xcode 项目由 XcodeGen 生成。主 App 与键盘扩展共用 `MARKETING_VERSION` 和 `CURRENT_PROJECT_VERSION`；商店更新说明放在 [`AppStore/release-notes/`](../../../platforms/ios/AppStore/release-notes/)。实际发布状态以 Apple 和公开资产读回为准。

## 检查与未签名归档

[`ios.yml`](../../../.github/workflows/ios.yml) 在匹配的 PR、main 推送或手动运行时检查发布配置、构建模拟器与设备目标，并运行 Shared 和 hosted iOS 测试。产品构建与自动测试在远端 CI 执行。

需要交给 Xcode 云签名的归档时，在 main 手动运行该工作流，设置 `archive=true` 和未使用的 `build_number`。工作流校验分发资源、按 iOS 版本生成未签名设备归档，保留 `ios-cloud-archive-<commit>-<build>` 产物及 `archive-provenance.json`。该路径不上传 Apple，也不生成可直接安装的发行包。

下载后核对源码、版本、构建号与 ZIP SHA-256，保留原归档。签名前从仓库根目录检查：

```sh
python3 platforms/ios/scripts/verify-release-ipa.py --check-unsigned-app <archive>/Products/Applications/RIMES.app
```

需要使用现有 Apple 云签名账号时，在归档副本上准备匹配的开发签名，再用 Xcode Organizer 的 Custom → App Store Connect → Export 导出；关闭自动管理版本号，保留主 App、扩展和共享组身份。导出的 App Store IPA 用同一工具检查：

```sh
python3 platforms/ios/scripts/verify-release-ipa.py <ipa> <version> <build>
```

检查同时覆盖签名、App Store profiles、版本、隐私声明及两目标的 App Group/Keychain。既有标识为 `org.scholay.rimes.ios`、`org.scholay.rimes.ios.keyboard`、`group.org.scholay.rimes.ios` 和 `585J2TL9U6.org.scholay.rimes.ios.shared`。运行时 Info.plist 与签名中的共享组必须一致，重新签名不会修复错误的运行时配置。

## 自动签名与 App Store 提交

[`ios-release.yml`](../../../.github/workflows/ios-release.yml) 由 `ios-vMAJOR.MINOR.PATCH` 标签触发。标签必须来自 main 历史并匹配 iOS `MARKETING_VERSION`；iOS 版本与根版本保持相同 major/minor，patch 不倒退。三语更新说明必须非空、每项不超过 4000 字符。构建号由 `(GITHUB_RUN_NUMBER + 100).GITHUB_RUN_ATTEMPT.0` 生成。

`ios-app-store` 环境需要以下 Secrets：

| Secret | 用途 |
|---|---|
| `ASC_KEY_ID`、`ASC_ISSUER_ID`、`ASC_PRIVATE_KEY_BASE64` | 具备发布权限的 App Store Connect API key |
| `IOS_DISTRIBUTION_P12_BASE64`、`IOS_DISTRIBUTION_P12_PASSWORD` | Apple Distribution 证书及私钥 |
| `IOS_APP_PROFILE_BASE64` | 主 App 的 App Store profile |
| `IOS_KEYBOARD_PROFILE_BASE64` | 键盘扩展的 App Store profile |

Profiles 必须匹配代码中的团队、bundle 与共享组，并在有效期内。凭据只通过发布环境配置，不写入仓库或日志；本指南不声明账号当前已配齐这些凭据。

工作流完成构建和测试后调用 [`fastlane/Fastfile`](../../../platforms/ios/fastlane/Fastfile)：使用临时 keychain 签名，验证 IPA，上传并等待 Apple 处理，再提交 App Review，设置审核通过后自动发布。它不发起外部 TestFlight 审核、不分发外部测试者，也不替换正在审核的版本或其他版本草稿。提交回执写入 `platforms/ios/build/ci-release/submission.json` 并进入 CI summary。

上传、Apple 处理、外部 TestFlight 审核、App Review 和商店可用是独立状态。处理超时后先读回 Apple 状态；重跑会产生新的构建号。清理签名材料的工作流仅用于一次性 GitHub 托管 runner。

## GitHub 社区 IPA

从原始未签名 CI 归档打包 `Payload/RIMES.app`，包含键盘扩展并保留执行权限和符号链接。附带 SHA-256、源码/CI 来源和自行签名说明；公开文件的可执行内容应与原归档一致。不要发布开发签名副本、设备 profiles 或凭据。

社区 IPA 需要用户签名，主 App 和扩展都必须有可用 profiles 以及共享 App Group/Keychain 权限。其发布不代表 Apple 已批准商店或 TestFlight 版本。商店资料入口见 [AppStore README](../../../platforms/ios/AppStore/README.md)，资源许可与隐私要求见[资源说明](resources.md)和[隐私说明](privacy.md)。
