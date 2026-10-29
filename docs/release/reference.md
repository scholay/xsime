# 发布技术参考

日常发布只需要看 [docs/release/README.md](README.md)。本文记录发布链路各环节的设计与约束，修改工作流、
安装器、更新器或签名配置前先读这里。

## 一、release.yml 的隔离与发布事务

tag 推送后，[`.github/workflows/release.yml`](../../.github/workflows/release.yml) 将构建、签名、安装验收和公开分到独立 runner：

- **`build_and_smoke`** 不读取发布 Secret。正式 tag 必须与当前 main、产品版本一致；服务端按固定 workflow ID、main push 事件及确切 SHA 复验五端最新 CI 成功，Android build 编号也须递增。
- **`sign_and_stage`** 进入受保护的 `macos-release`，先被动检查交接 app 的成员、路径、权限、大小、版本与摘要，再导入 Developer ID 凭据。它不执行交接 app，公证和复核后先销毁临时 keychain/P8，再上传不可变 signed-stage。
- **`verify_signed_install`** 在另一台一次性 Mac 上安装/升级同一签名包，校验实际安装身份和数据保留；这不等于所有真实用户宿主都已验收。
- **`build_windows_release` / `build_android_release` / `build_linux_deb`** 各自产出同一提交的完整包与验证结果；Linux 包先完成安装、升级、重装和卸载检查。任何生产任务未通过都不能进入正式公开。
- **`sign_update_manifest`** 使用独立的受保护 `macos-release` job。准备步骤只从当前 tag run 取不可变包，核验 run/SHA/artifact ID/digest、安全解包与包身份；签名步骤只接触生成的完整清单，不执行包。
- **`publish_staged_release`** 进入无签名密钥的 `macos-publish`。它检出确切 tag 的已审核发布 helper，被动复核 macOS stage 的四个成员、四端包与签名清单；不重建、重签、重公证，也不执行下载产物。
- **`publish_unsigned_preview`** 维持无发布密钥的 Pre-release 通道；`attach_linux_deb` 只为该预览附加 Linux 包，不参与正式 Release 的后置补包。

macOS 构建与封存：

1. arm64/x86_64 分别用 SwiftPM 原生构建，再以 `lipo` 合并；两架构资源 bundle 集合及字节必须一致。
2. 固定的 librime 运行时、引擎插件与词库进入 `RIMES.app`；ad-hoc smoke 与正式签名字节分别验收。
3. Developer ID Application 重签 bundled Mach-O 和 app，Developer ID Installer 签精确 `RIMES-X.Y.Z.pkg`；只提交该 PKG 公证、等待 Accepted 并 staple，不创建 App ZIP。
4. 最终 PKG 及展开的 payload 通过 `codesign`、`pkgutil`、`stapler`、`spctl` 后，销毁临时凭据；将 pkg、`SHA256SUMS`、`RELEASE-NOTES.md`、`release-manifest.txt` 四个成员作为 signed-stage 上传。ID、run、源码与压缩包摘要绑定该不可变 Artifact。

正式发布事务由 [publish_update_assets.py](../../scripts/release/publish_update_assets.py) 统一执行：

1. 核验四端包的 source/version、checksum 与签名清单中的 URL、大小、SHA-256；过期或无效清单直接拒绝，不自动重签。
2. 在 `stable-release-publication` 互斥组内，检查没有更高正式版本，Android versionCode 高于前一正式版本；恢复已公开同版本时只接受原编号。
3. 创建 draft，或匹配带有相同 run/commit/资产集合摘要的既有 draft；来源不同、额外资产或已有不同字节都拒绝，绝不覆盖。
4. 先核对全部已有资产，再补上传缺失资产；所有包、元数据与 `update-manifest.json` 都下载读回比较。此时旧 Latest 仍有效。
5. 再次核对 tag 和版本单调性，然后一次将 draft 公开并设为 Latest。若服务端已公开但响应丢失，重试只读回完整资产，不重新发布或把旧版本设回 Latest。

恢复入口 [complete-release-assets.yml](../../.github/workflows/complete-release-assets.yml) 必须使用 `--ref vX.Y.Z` 从原稳定 tag dispatch，
调用者的 repository、tag、commit 与原 source run 必须匹配；检出的 helper 来自该冻结 tag。它需要新的 `macos-publish` 批准并使用同一个互斥组，不放宽环境的 `v*` tag 限制。
原构建、签名、清单签名及同包安装任务必须全部成功；不再要求此前发布 job 成功，因此可恢复“尚未创建/部分 draft/公开后读回失败”。
恢复仅消费未过期、身份唯一的原 Artifact；无法证明来源、发现不同字节、签名过期或已有更新版本时拒绝，不用新构建或新签名偷偷替换旧 draft。

正式 tag 在签名前必须精确指向当时 main，发布前重新核对 tag 未移动。允许同源 draft 续接，但不允许重用不同资产的版本号。
预览 tag 必须来自 main 历史，Pre-release 保持 `latest=false`；PR、定时及手动演练不公开，默认采用产品目标版本并保留临时 Artifact。
所有外部 Actions 固定完整 commit SHA；各 job 仅授予自身所需读写权限。真实设备验收按维护规则另行记录。

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
`macos-publish`。发布 job 执行确切 tag 的已审核发布 helper 和系统校验工具，仅被动检查交接包；它先认证完整 draft，随后公开并读回，不能用开发 build/smoke 或普通演练 Artifact 替代。

## 七、应用内自动更新（[`UpdateManager.swift`](../../Sources/RimeBuffer/UpdateManager.swift)）

### 独立清单签名与兼容

四端更新清单保留 `schemaVersion: 1`、版本、平台资产和来源提交等旧字段，新增签名有效期与
`authentication`。旧客户端仍能读取原字段；新客户端在信任版本、地址或摘要前验证签名，缺签名、
无受信任密钥、内容不一致、过期或版本回放都拒绝，不降级到旧的无签名路径。iOS 继续通过 App Store。

签名为 P-256/SHA-256，覆盖 `authentication.payload` 中的原始 UTF-8 JSON 字节；签名编码为
64 字节 `r || s` 的 Base64。解析后的签名内容必须与去掉 `authentication` 的顶层清单一致。
公钥唯一来源为 `Shared/AppUpdate/trusted_keys.json`，运行
`python3 -B scripts/release/sync-update-trust.py` 生成 Swift、Java、C++ 公钥常量；CI 检查三端一致性。
共享公开测试向量不属于产品信任根，测试私钥不保存到仓库。

受保护的 `macos-release` 环境增加 `RIMES_UPDATE_SIGNING_KEY_PEM_BASE64`，内容为更新专用
P-256 PEM 私钥的 Base64。私钥只在独立清单签名步骤可见，以 0600 临时文件使用并在结束时清理；
该 runner 不执行下载的安装包。签名清单、应用签名和公开发布各自经过环境批准，不把 GitHub
公开资产上的 SHA-256 当成独立的发布者身份认证。

清单最长有效 90 天，允许 5 分钟时钟偏差。新客户端会在检查、下载和安装前复验有效期，并保存
已验证的最高版本；Android 同时保存最高构建号。长期停更导致清单过期时应明确提示不可核验，
不能继续安装过期包。恢复发布复用原签名清单；到期后需发布更高的正式版本，不能覆盖旧资产续签。

轮换密钥时先将新公钥加入受审查的信任表，保留旧公钥；可用
`RIMES_UPDATE_SIGNING_PREVIOUS_KEY_PEM_BASE64` 对同一清单双签，至少一条受信任签名成功即可。
旧客户端尚未获得新公钥时必须保留旧签名的桥接发布，不要直接换成它们不认识的单一新密钥。
删除旧信任根、密钥丢失或泄漏需要独立评估已有客户端的升级通路。

独立清单签名不会把 Windows EXE 变成 Authenticode 已签名文件，也不替代 macOS 公证、Android
生产证书或 Linux 包身份检查。对应系统检查继续执行；版本号、签名身份和用户数据契约不变。

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
