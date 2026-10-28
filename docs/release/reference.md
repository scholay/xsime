# 发布技术参考

日常发布只需要看 [docs/release/README.md](README.md)。本文记录发布链路各环节的设计与约束，修改工作流、
安装器、更新器或签名配置前先读这里。

## 一、release.yml 的三段隔离设计

tag 推送后，[`.github/workflows/release.yml`](../../.github/workflows/release.yml) 使用彼此隔离的 runner：

- **`build_and_smoke`** 不绑定受保护 Environment，也不读取任何发布 Secret。它构建并实际执行 ad-hoc
  签名 app 的 runtime smoke，再把 app、构建上下文和校验和作为短期 handoff Artifact 传出。
- **`publish_unsigned_preview`**（`vX.Y.Z-preview.N`）是另一台无 secrets 的全新 runner，只做被动的成员集、
  权限、版本、架构、签名状态与 SHA-256 复核，不执行 handoff 中的 app，然后创建 Pre-release。
- **`sign_and_stage`**（`vX.Y.Z`）是全新 runner，才进入受保护的 `macos-release` Environment。它先按严格的
  成员 / 路径 / 权限 / 大小规则解包 app 并被动校验，通过后才导入签名与公证凭据；凭据所在 runner
  不执行 handoff 中的 app。签名、公证、staple 和最终资产复核通过后，它先销毁临时 keychain / P8，再产生
  不可变 signed-stage Artifact。
- **`publish_staged_release`** 是第三台全新 runner，进入不含任何签名/公证 Secrets 的受保护
  `macos-publish` Environment。required reviewer 必须先审阅维护者的真机验收记录；该 job 只认证 stage 的
  artifact ID/digest/运行号/commit，逐字节复核四个成员并从中发布，绝不 checkout、重建、重签或重公证。
- **`build_linux_deb`** 在无发布权限的 Ubuntu 24.04 amd64 runner 上构建同一版本的 deb，并验证安装、
  重装、卸载与用户数据保留。两个公开发布 job 都等待它成功；随后 **`attach_linux_deb`** 向已公开的
  同名 `v*` Release 添加 deb 与单独的 `.sha256`，下载读回比对，不覆盖已存在的不同字节。

构建步骤：

1. arm64 与 x86_64 分别用 SwiftPM 原生构建系统构建，再 `lipo` 合并为**通用二进制**。一次传两个
   `--arch` 会切到 Xcode 构建系统，它无法解析 SwiftTerm 1.20 的 build-tool plugin；两边的资源 bundle 必须
   集合一致、逐字节相同，只拷一份。
2. `scripts/fetch-rime.sh --force` 下载 librime 运行时（见第三节）。
3. 组装 `RIMES.app`：二进制 → `MacOS/RIMES`，`Info.plist`，librime + 插件 + Rime 词库拷进
   `Contents/Frameworks` 与 `Contents/SharedSupport`；ad-hoc 签名后执行 runtime smoke。
4. 预览版与演练：打未签名 PKG，在一次性 runner 上先安装最新已发布 Release，再真实执行
   `sudo installer` 安装新包，验证 PackageKit 退出状态、固定路径、输入法 metadata、通用架构、安装回执，
   以及升级后 `/Library/Input Methods` 只剩 `RIMES.app` 一个 RIMES 身份。
5. 正式版：用 Developer ID Application 证书逐个重签所有 bundled Mach-O，再以 hardened runtime
   签 app；以 Developer ID Installer 签署 `RIMES-X.Y.Z.pkg`。用户实际接收的最终 PKG 是最外层分发容器，
   因此只提交该 PKG 公证、等待 `Accepted` 并 staple；不创建或发布可直接解压的 App ZIP。
6. 对最终 PKG 及其展开后的 Developer ID 签名 payload 执行 `codesign`、`pkgutil`、`stapler` 与 `spctl`
   校验；随后销毁临时 keychain / P8，才把 pkg、`SHA256SUMS`、`RELEASE-NOTES.md` 与严格 manifest 作为
   four-file signed-stage 上传。第二道 job 使用受控的 `gh` CLI 从该 stage 创建仅含 pkg 与 `SHA256SUMS` 的
   Release，并下载正式资产读回校验；所有外部 Actions 都固定到完整 commit SHA。

`build_and_smoke` 执行的是 ad-hoc smoke bundle；正式 runner 会在之后重新签名、重新打包和公证。
因此 build / smoke 成功不能替代对正式字节的验收：正式 workflow 会被动校验最终 pkg 的签名、票据和
Gatekeeper 策略。signed-stage 出现后，维护者必须下载其中的**精确** `RIMES-X.Y.Z.pkg`，而不是用 checkout
重打包或普通演练 Artifact，并在批准 `macos-publish` 前通过 `scripts/rehearse-release-pkg.sh` 做真实安装。
发布器会证明随后公开的 Release 资产与该 stage 是同一批字节。

授权规则：

- 正式版 tag 必须在签名前精确指向当时的 `origin/main`；签名完成后 tag 不得移动，stage 与发布前各复核
  其 commit，Release 必须仍为空位。主线随后有新提交不会改变已审计 tag 的字节。
- 预览版 tag 必须指向已在 main 上的提交，tag 在发布前不得移动，Release 必须仍为空位。
- 预览版 Release 的 macOS 资产为 `RIMES-X.Y.Z-preview.N.pkg` 与 `SHA256SUMS`；Linux job 随后附加
  `fcitx5-rimes_X.Y.Z~preview.N_amd64.deb` 与其 `.sha256`。Release 标记为 Pre-release、`latest=false`，
  正文写明 macOS unsigned / not notarized，链接 `docs/release/unsigned-preview.md` 并附 Linux 安装说明。
- PR、定时和手动运行都是演练：版本号默认取下一个预览版本，只上传保留 7 天的 Artifact。

<a id="release-environment"></a>

## 二、正式发布环境与 Secrets

> Apple Developer Program 资格或开发机上可见的一张 Developer ID identity，都不是正式发布已就绪的
> 证据。正式 `vX.Y.Z` 发布前必须完整配置下述两张证书及私钥、受保护的 `macos-release` 与
> `macos-publish` Environment 和全部公证凭据，并让正式 workflow 成功生成、校验和发布签名公证的 pkg。未满足这些条件的 macOS 包只能发布
> 未签名 Pre-release，绝不能标记为正式 Release 或 Latest。首个正式版必须使用高于所有既有版本的
> 新版本号，绝不能在已发布版本上补签后覆盖原字节。

先创建两个受保护的 GitHub Environment：

- `macos-release`：签名/公证门，必须配置至少一位 required reviewer、**禁止
  administrator bypass**，并以 selected branch/tag policy 允许 `v*`；这里才保存下表八项 Secrets。
- `macos-publish`：公开发布门，也必须配置上述 reviewer / admin-bypass / `v*` policy，且
  **不要配置任何 Developer ID 或 notary Secret**。GitHub 的 required-reviewer 规则只要求名单中的一人批准；
  两个 Environment 可以使用同一个 reviewer，由其分别批准签名和发布，不要求两位独立审核者。

当前采用单维护者流程：两个 Environment 的 required reviewer 都配置为 `scholay`，
`prevent_self_review=false`，允许该账号发起发布并分别批准签名与公开。两次人工审批和禁止 administrator
bypass 仍然保留，公开审批前仍必须完成同一签名安装包的真机验收。这不是两个人的独立复核；若团队以后需要
独立复核，再启用 prevent self-review，并配置不同的人或不重叠的 reviewer team。

正式工作流在导入凭据和公开 Release 前，会通过 GitHub API 重新断言以上 Environment 控制；缺失、允许
admin bypass、没有 reviewer 或没有 `v*` policy 都会 fail-closed。工作流不要求两个阶段使用不同的
reviewer；若另行采用团队独立复核策略，成员分离由仓库管理员配置。

独立的 `release tag creation` ruleset 覆盖 `refs/tags/v*` 的 **creation**，只给发布账号 `scholay` bypass；
原有 `release tags` ruleset 无 bypass，继续禁止所有人删除或移动已有 tag。两类规则分开配置，避免创建权限
同时变成覆盖已有版本的权限。GitHub Actions Artifact 在公开仓库中的访问规则不是发布禁运；它只是发布
权威门，用户和更新器的唯一来源仍是正式 GitHub Release。

在该 Environment 的 secrets 中配置以下 8 项；P12 必须分别包含有效的 Developer ID Application /
Developer ID Installer 证书及其私钥：

| Secret | 内容 |
|---|---|
| `RIMES_DEVELOPER_ID_APPLICATION_P12_BASE64` | Application `.p12` 文件的单行 Base64 |
| `RIMES_DEVELOPER_ID_APPLICATION_P12_PASSWORD` | 上述 P12 的导出密码 |
| `RIMES_DEVELOPER_ID_INSTALLER_P12_BASE64` | Installer `.p12` 文件的单行 Base64 |
| `RIMES_DEVELOPER_ID_INSTALLER_P12_PASSWORD` | 上述 P12 的导出密码 |
| `RIMES_DEVELOPER_TEAM_ID` | 两张证书所属的 10 位 Apple Team ID |
| `RIMES_NOTARY_KEY_P8_BASE64` | App Store Connect API 私钥 `.p8` 的单行 Base64 |
| `RIMES_NOTARY_KEY_ID` | App Store Connect API Key ID |
| `RIMES_NOTARY_ISSUER_ID` | App Store Connect Issuer ID（UUID） |

> 可用 `base64 -i certificate.p12 | tr -d '\n'` 生成适合粘贴的单行内容；P8 同理。
> 不要把证书、私钥或密码提交到仓库。

导出的 P12 应同时带上与签名证书匹配的 Apple 中间证书（当前为 Developer ID G2），
避免依赖开发机已缓存而 fresh runner 未安装的证书链。只使用
[Apple PKI](https://www.apple.com/certificateauthority/) 的官方中间证书；不要把叶证书设为“始终信任”。
导入脚本按照 [GitHub macOS 签名指南](https://docs.github.com/en/actions/how-tos/deploy/deploy-to-third-party-platforms/sign-xcode-applications)
显式将临时 keychain 加入 user search list，保留已有项和默认 keychain，并另外检查 Application identity
通过 `codesigning` policy，而不把默认 basic X.509 检查当成可签名的证明。

正式 workflow 只在 fresh signing runner 把两张证书导入一次性 keychain，只把证书 common name、
Team ID 和临时路径传给后续步骤；P12 导入后立即删除，公证结束、signed-stage 上传前删除临时 keychain 与 P8；
失败路径也会无条件清理。导入脚本拒绝多张同类型 identity、Team ID 不一致或缺少任一凭据的配置。

## 三、自包含 librime（[`scripts/fetch-rime.sh`](../../scripts/fetch-rime.sh)）

librime 是**静态链接**的（依赖只有系统 libSystem/libc++），所以自包含只需三样：
`librime.1.dylib` + 3 个插件 + `SharedSupport`（默认方案/词库）。fetch-rime 从 Squirrel
官方 `.pkg` 提取这些到 `Vendor/rime/`。1.1.2 的 URL、字节长度、SHA-256、Developer ID
Installer 身份/Team、公证票据，以及四个 universal dylib 的独立 SHA-256 都固定在脚本中；
任何一项漂移都会在展开和 RIMES 重签之前 fail-closed。发布工作流总是 `--force` 下载，
不信任 runner 上已有的 `Vendor/` 缓存。升级 Squirrel 必须在 PR 中重新审计并更新 allowlist。

简体 Octagram 模型保持为已审计的 40,925,228-byte compact 字节。获取时先复用
`RB_OCTAGRAM_MODEL_PATH` 指定文件或 `Vendor/.cache`（两者都重新校验长度和 SHA-256），
再以官方 `rime-octagram-data` 20260712 Release 资产为主源、固定 revision 的 raw 文件为
备源；每个网络源都有有界重试和超时。所有模型来源在写入缓存和替换现有
`Vendor/rime` 之前完成最终校验，因此失败不会破坏上一次可用 runtime。`--force` 会跳过
本地来源，正式发布仍只接受从已审计网络源重新取得的完全相同字节。

- **`Vendor/` 是 gitignore 的**——二进制不进 git，构建时按锁定版本拉取，可复现。
- 运行时 `CRimeBridge` 优先 `dlopen` app bundle 内的 librime（找不到才回退系统 Squirrel），
  `shared_data_dir` 指向 bundle 的 `SharedSupport`；首启自动 `start_maintenance` 部署词库到
  `~/Library/RIMES`。因此 CI 与终端用户机器都无需预装 Squirrel。

## 四、预置缓冲插件 catalog

预置缓冲插件名称、版本与默认安装策略统一维护在 `Catalog/buffer-plugins.json`。每次插件更新先运行
`python3 -B scripts/sync-buffer-plugin-catalog.py`，它会同步运行时 Swift catalog 与中英文 README 表；
CI 和 `scripts/release.sh` 都会用 `--check` 阻止版本或文档漂移。当前维护的三个 Buffer 插件随应用一起
交付，不再生成或上传独立 manifest / 插件资产。历史 Release 中的旧插件资产按不可变发布政策保留，
但不代表当前产品仍维护或安装它们。

## 五、持续集成

| 工作流 | 触发 | 内容 |
|---|---|---|
| `CI`（[ci.yml](../../.github/workflows/ci.yml)） | push / PR 到 main | 发布工具单元测试、`release.sh` 语法、`Info.plist` 版本占位、PR 提交信息、公开 Release 驱动的 CHANGELOG 警告；以及 macOS `swift build` 与纯 Swift smoke |
| `Platform Preview Data` | 每周 / 手动 | 跨平台数据闭包校验与 Windows / Linux 包事务；维护性检查，不阻断 macOS 发布 |
| `Windows Native Foundation` | 每周 / 手动 | Windows 原生 x64 / x86 构建与测试；维护性检查，不阻断 macOS 发布 |
| `Windows IME` | 相关路径的 PR / push 到 main、手动 | x64 / x86 构建、Fake TSF + 真 Broker 打字测试和独立 Artifact；不是安装包发布 |
| `Linux IME` | 相关路径的 PR / push 到 main、手动 | Fcitx5 / Buffer / Capsule 构建与 headless E2E、源码安装卸载、deb 安装／重装／卸载与 Artifact |
| `Linux deb package`（可复用） | `Linux IME` 与 `Release macOS` 调用 | Ubuntu 24.04 amd64 deb + SHA-256；`v*` 发布成功后附到同一 Release，演练只保留 Artifact |
| `iOS checks` | 相关路径的 PR / push 到 main、手动 | 共享测试、iOS 构建和模拟器测试；无商店提交 |
| `Release iOS to App Store` | `ios-v*` tag | 必须来自 main 历史；独立签名、build number 与审核流程，见 iOS CI_RELEASE.md |
| `Release macOS`（演练） | 打包相关 PR、每日定时、手动 | 完整通用构建、打包、从最新 Release 升级安装，不发布 |
| `Release macOS`（发布） | `v*` tag | 见第一节 |

main 的 ruleset 只要求 `CI` 的 `Release tooling` 与 macOS `build` 成功；`Release macOS` 演练带路径过滤，
不作为必需检查，由定时运行兜底。Windows / Linux IME 的 PR 检查提供平台证据，但不加入 macOS
tag 创建门禁；`v*` 发布工作流内的 Linux deb 构建及包测试必须成功，才公开组合 Release。
Linux 桌面验收仍须单独记录。完整维护约定见 [docs/development/maintenance.md](../development/maintenance.md)。

## 六、安装器（[`scripts/make-pkg.sh`](../../scripts/make-pkg.sh)）

`.pkg` 会把 `RIMES.app` 固定安装到 `/Library/Input Methods`，且显式禁用 Installer
relocation。`BundleHasStrictIdentifier=false` 是刻意的兼容策略：改名期间的开发构建曾以
`com.scholay.isaac` 使用 `RIMES.app`；preinstall 会先拒绝任何非 RIMES 占用者。改名前的
`ETInput.app`（`com.isaac.inputmethod.RimeBuffer`，v0.4.1 及更早为
`com.isaac.inputmethod.ETInput`）在另一路径：payload 落盘后，postinstall 扫描系统与当前
GUI 用户的 `Input Methods`，移除除新 payload 外所有声明 RIMES 身份的 bundle；若仍有重复
路径无法移除或扫描失败，postinstall 记录路径并以失败退出，不启动 companion。

替换 payload 前，preinstall 使用包内随新版本构建、签名的最小 universal TIS helper 切到
ASCII fallback，不再要求 v0.4.1 等旧 binary 理解新参数。所有可能碰到 DirectoryService、
网络 home、Aqua bootstrap 或 TIS 的命令都由独立进程组 watchdog 约束；TERM/KILL 会覆盖整棵
命令树，preinstall/postinstall 另有 30/240 秒总预算。payload 落盘后，postinstall 在当前
GUI 用户的 Aqua 会话中按独立子进程分阶段执行 `register → enable parent →
enable child → best-effort select`。注册/启用在 90 秒总预算内未收敛时，pkg 仍成功，
并为当前用户安排一个仅在下次 Aqua 登录执行的 one-shot repair LaunchAgent；
成功后 marker 与 LaunchAgent 会自删除。开发安装另用一个不带 `KeepAlive` 的用户级
companion LaunchAgent：新定义在同一目录完成构造、lint 与字段校验后原子替换，只执行一次
`open -g` 用户 App。系统包在替换 payload 前枚举所有本机普通账户的
准确 home 记录：只允许当前 GUI 用户存在可验证、可由 postinstall 退休的 dev app/agent；
任何其他账户存在同 ID dev 安装、home 无法安全遍历或记录不一致都会在 payload 改动前
fail-closed。postinstall 退休当前用户的 dev 安装后再次全量审计，再发布另一个系统级 Aqua
LaunchAgent。它在每次冷登录时只运行一次短命 guard：若后来又出现用户级 dev 痕迹则直接
退出，否则执行 `/usr/bin/open -g /Library/Input Methods/RIMES.app`；它不设 `KeepAlive`，
也不承载第二份 UI/IME 服务。这个登录 guard 只是对安装后异常残留的防御，不能代替包安装前
的全账户冲突审计。postinstall 在替换这个 system agent 前保存原始字节，后续
旧进程退出或新进程启动检查失败会恢复旧 agent（原来不存在则恢复为不存在）。这样即使
登录后选用其他输入法，Buffer、Clipboard、Mailbox 与 Capsule 的 Carbon 快捷键仍由同一
个 RIMES 进程提供。无 GUI 用户时仅在所有本机账户都无 dev 冲突后安装 payload 与冷登录
bootstrap；TIS 激活留到 GUI 会话建立后处理。任何路径都不结束
`imklaunchagent`/`TextInputMenuAgent`。

发布回归还必须固定周边窗口的权限边界：在其他输入法下，四个全局快捷键仍可开关对应窗口，
但不得访问外部 IMK client、切换输入源、合成粘贴/其他按键、调用 Accessibility/Post Event，
或读取、提交、取消外部输入法组字。设置窗口只在 RIMES 输入源下打开；其中 Mailbox 与 Capsule
必须是配置/状态页，不能嵌入实际会话或内容管理 pane。Capsule Password 的查看入口必须显示
四个槽位，并用原生物理键事件验证四组 chord（默认 `RH / WO / CVN / QU`）；更换和恢复默认
都先验证当前凭据，自定义原码不落盘或同步，只保留一个加盐摘要凭据，验证成功后的明文最多
显示 15 秒。

输入法 bundle id 为 `com.scholay.inputmethod.isaac`，可选择的输入模式使用独立 id
`com.scholay.inputmethod.isaac.Hans`。必须保留 `.inputmethod.` 段：改名期间的
`com.scholay.isaac` 虽然签名校验及注册调用成功，却无法进入 TIS 输入源列表。
签名证书与 Bundle ID 是两个独立设置；更换证书不要求更换 Bundle ID。旧标识的
偏好（`RimeBuffer.` 前缀）与 `~/Library/RimeBuffer` 数据目录各按独立迁移标记一次性复制到新名称，
只复制不移动，原目录保留作回退。父输入法与
子 mode 不能共用同一个 TIS id，否则父项无法启用、`TISSelectInputSource` 会返回 `paramErr`。
macOS 会把这些 id 写入受保护的 TIS 偏好，因此后续不要随意改动。

### 开发安装与正式包安装：二选一

`build_install.sh` 是开发维护通道：它从 checkout 构建并把可变的开发版安装给当前用户；它不是
正式资产，也不能用于证明正式 Release 的安装质量。普通用户的正式包通道只接受 GitHub 正式 Release
中下载的精确 `RIMES-X.Y.Z.pkg` 与 `SHA256SUMS`，由 Installer 安装到 `/Library/Input Methods`。
同一用户或正式包测试机一次只能选择一条通道；不要让开发安装覆盖正式包。

在第二道批准前，维护者可从 signed-stage 下载同一份 pkg，依次运行：

```bash
scripts/rehearse-release-pkg.sh --pkg /absolute/path/RIMES-X.Y.Z.pkg
scripts/rehearse-release-pkg.sh --pkg /absolute/path/RIMES-X.Y.Z.pkg --install-gui
```

默认命令只做签名、公证、Gatekeeper、payload 与架构检查；`--install-gui` 才会打开与用户相同的 Installer，
并在安装后验证固定路径、回执和真实输入源。它会退休当前 GUI 用户的开发 app / agent，因此必须使用测试账号
或可恢复测试 Mac。记录 tag、stage asset SHA-256、macOS 版本和测试设备；reviewer 审阅这些证据后才批准
`macos-publish`。发布 job 不执行本地 checkout 代码，只认证已封存 stage 并读回公开 Release；这两层都不能
用开发 build/smoke 或普通演练 Artifact 替代。

## 七、应用内自动更新（[`UpdateManager.swift`](../../Sources/RimeBuffer/UpdateManager.swift)）

已安装并运行的正式签名 RIMES：

- **启动时 + 每小时** 静默查询 `scholay/xsime` 的最新 Release；
- 只接受严格 `vX.Y.Z` 下唯一的 `RIMES-X.Y.Z.pkg`，并在后台下载；
- 下载完成后，输入法菜单中的更新项变为「安装 RIMES vX…」；
- 下载后和用户确认后各验证一次：GitHub HTTPS/精确资产路径、普通文件/大小上限、
  `pkgutil --check-signature`、`spctl --assess --type install`，以及 Installer 证书与当前 app
  的 10 位 Team ID 一致；每个系统校验工具都有硬超时，且包内 product/component identifier
  与版本必须精确等于 Release 的 `X.Y.Z`，不能把历史同-Team pkg 改名重放；
- 用户确认后只把已验证 `.pkg` 交给 macOS Installer。应用自身不修改
  `/Library/Input Methods`，不解压任意 app，不清 quarantine，也不结束进程；
- 也可从菜单「检查更新…」手动触发。自动检查默认开启（`UserDefaults` 键 `updateAutoCheckEnabled`）。

版本比较只接受严格的 `X.Y.Z`：当前版本或 Release 版本不是这个形状（预览版、`0.0.0-dev` 占位值、
`git describe` 命名的开发版）时，不会提示更新；只有 tag 版本**严格大于**当前运行版本时才提示。
正式 Release 不包含 `RIMES-X.Y.Z.zip`；更新器只消费精确、已签名并已公证的 PKG。

## 八、Windows / Linux 输入方案预览

跨平台 Data / Input-Schemes Preview 使用独立的 `platform-preview-vX.Y.Z` 标签和
`.github/workflows/platform-preview-release.yml`，由维护者显式运行 `./scripts/release.sh platform
patch|minor|major|X.Y.Z` 创建。日常数据验证由只读的 `.github/workflows/platform-preview.yml` 每周或手动运行；
它们不会成为 main、macOS 预览或正式 macOS Release 的门禁。该标签不会匹配 macOS 发布所用的 `vX.Y.Z` 规则；
生成的 GitHub Release 必须标记为 **Pre-release**，因此也不会进入 macOS 客户端查询的 `/releases/latest`
自动更新通道。

预览工作流在 Windows、Linux 与 macOS runner 上共同校验审核过的 Rime 数据闭包，并在
原生 Windows/Linux runner 上执行安装、校验、卸载文件事务。发布资产只是面向
Weasel、Fcitx5 Rime 与 IBus Rime 的数据/脚本包，不得描述为完整原生 RIMES 应用。
精确能力边界、被排除的数据和本地验证命令见 [docs/release/platform-preview.md](platform-preview.md)。
