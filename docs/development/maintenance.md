# 主线与多平台维护

本页列出五端共同维护规则、代码边界与版本来源。

## 原则

- 一个仓库、一条应用集成主线 `main`；macOS、iOS、Android、Windows、Linux 不维持长期分叉的产品主线。
- 新工作从最新主线开短分支，一件事一个可审查提交/PR；合入后不再沿旧分支叠加功能。
- 每个平台独立构建和验收。macOS、Android、Windows、Linux 的正式安装包统一由项目 `v*` 流程构建并附到同一 Release，全部读回校验后才发布共同的更新清单。iOS 独立走 App Store；各平台构建编号独立递增。合入主线不等于可用或已发布。
- 本地维护可以领先 `origin/main`。未获授权时，不 push、不创建发布 tag、不触发远端 workflow，
  不安装输入法、不提交 App Store、不修改远端 PR 或保护规则。
- 获准同步远端时，从已验证的本地主线提交创建交付分支，经 PR 回到 `origin/main`；不 force-push
  或绕过保护。再按届时的远端主线和 CI 状态处理本地对齐，不直接重置包含未发布工作的分支。
- 用户词库、凭据、运行时数据不参与分支整理；保留原始提交与备份引用，不自动清理旧分支。
- 产品构建与自动测试走远端 CI，维护者本机不运行产品构建或测试。记录确切提交的 CI 结果，不能把旧 CI、旧安装包或历史审核状态沿用到新提交。
- 真实宿主和设备验收与自动检查分别记录；每项验收至少包含平台、完整 commit、构建配置、检查结果和未验证项。合并、编译或 CI 通过不能单独作为问题关闭依据；修复应关联检查结果、真实宿主确认与所属发行版本。
- 仍可复现的普通输入失败、丢字或重复上屏、焦点串投、用户词库损坏、明显性能退化，以及本版承诺设置无法生效或恢复，阻断发布。未确认根因的反馈先分诊，未纳入本版的长期功能不自动阻断发布。

## 代码边界

五端通过行为规范、数据格式和测试样例对齐；各平台保留自己的宿主输入契约与安全边界。构建和真实宿主验收分别记录。

| 范围 | 维护入口 | 验证责任 |
|---|---|---|
| macOS | `Sources/`、`Package.swift` | IMK 焦点/投递/保护态、原生 UI、升级安装 |
| iOS | `platforms/ios/` | App 与键盘扩展、Full Access、设备与 App Store |
| 共享 Swift 逻辑 | `Shared/` | 共享单测 + macOS/iOS 受影响路径 |
| Windows | `platforms/windows/native/` | TSF/Broker、x64/x86、真实 Windows 宿主 |
| Android | `platforms/android/` | App/IME、InputConnection、JVM 测试、APK 与真机 |
| Linux | `platforms/linux/` | Fcitx5、Buffer/Capsule、X11/Wayland 真实桌面 |
| 共享词库/配置 | `rime-data/`、`chord-keymaps/`、`Catalog/` | 目录生成检查、数据闭包及受影响平台 |
| 设计 | `DesignSystem/` | 单独提交；界面原型不冒充产品验收。宣传视频的工程、素材与成片不进本仓库 |

Windows/Linux 的 C++ 与 Android 的 Java 适配层不直接共享 Swift 实现。
平台安全边界仍由各自宿主负责；macOS 的 `Delivery.insert`、用户词库隔离等约束不因整合改变。

## 官方插件依赖

官方插件源码由独立仓库 `scholay/rimes-plugins` 管理。本体的 `OfficialPlugins` 子模块与 `plugins.lock.json` 固定同一个已审查提交；原生源码随本体编译和签名。下载包只提供宿主支持的声明、提示词和资源。

首次检出以及切换依赖提交后运行：

```bash
git submodule update --init OfficialPlugins
python3 scripts/prepare-official-plugins.py
```

生成文件不在本体重复维护。修改插件源码须在插件仓提交，再更新本体子模块及锁文件；导入器会拒绝覆盖本地改动。

## 版本来源

| 平台/渠道 | 版本来源 | 构建与发布入口 |
|---|---|---|
| macOS | 根 `VERSION` 与 `Info.plist`；正式 tag 必须匹配 | `CI` / `Release macOS`、`scripts/release.sh` |
| iOS | `platforms/ios/project.yml`、独立 build number；App Store 渠道使用 `ios-vX.Y.Z` | `iOS checks` / `ios-release.yml` |
| Android | `platforms/android/VERSION` 与 Gradle versionCode | `Android checks` / 项目 `v*` 正式流程 |
| Windows 原生 | `platforms/windows/native/VERSION` 生成工程、PE 和运行时版本 | `Windows IME` / 项目 `v*` 正式流程、`New-RimesNativePackage.ps1` |
| Linux 原生 | 根 `VERSION`；正式包读取同一 `v*` tag，preview 转为 Debian `~preview.N` | `Linux IME` / `linux-deb.yml` / 项目 `v*` 正式流程 |
| Windows/Linux 数据预览维护通道 | `platform-preview-vX.Y.Z` | `platform-preview-release.yml`；数据包不等于原生 IME 产品版本 |

四端正式安装包使用同一项目 `v*` 版本，各平台构建编号独立；iOS 的上传、审核和上架状态单独记录。已发布标签与资产不删除、移动或覆盖。当前版本、系统支持范围、签名状态和验收证据以[发行记录](../releases/)为准，流程见[发布指南](../release/README.md)。

Windows/Linux 的日志版本、CMake 项目版本、包版本升级时一起核对；Linux CMake 与 deb 统一读取根 `VERSION`。修改安装、依赖、升级或卸载行为时，同步更新平台安装文档与发布说明，并保留用户词库、学习记录和其他持久化数据。

PR、手动及定时构建产物保留为 Artifact；它们的存在不代表安装、真实桌面或签名验证完成。正式发布只使用确切提交的构建结果，并在安装包和公开资产读回后记录完成状态。
