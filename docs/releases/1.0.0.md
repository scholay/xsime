# RIMES 1.0.0 发布记录

2026-10-04，北京时间。按维护者授权先公开二进制，新增应用源码继续留在本地。Linux 不纳入本轮。

| 平台 | 发布状态 | 入口 |
|---|---|---|
| macOS 1.0.0（build 1） | 正式发布，GitHub Latest；通用 arm64/x86_64 PKG 已签名、公证、附票并通过 Gatekeeper | https://github.com/scholay/rimes/releases/tag/v1.0.0 |
| Android 1.0.0（code 9） | 正式发布；长期密钥签名 APK / AAB；小米 17 Pro / Android 16 新装、启动及拼音上屏通过 | https://github.com/scholay/rimes/releases/tag/android-v1.0.0 |
| Windows 1.0.0（PE 1.0.0.0） | 正式发布；按维护者要求提供未签名 EXE；包内容与哈希核验通过 | https://github.com/scholay/rimes/releases/tag/windows-v1.0.0 |
| iOS 0.1.0（32） | 当前对外提供的 TestFlight 版本；已通过外部测试审核，RIMES Community 公开邀请已开启，可加入并安装 | https://testflight.apple.com/join/Kdj9RB4q |

## 源码边界

三个 GitHub Release 标签均指向已有公开提交 `54701c9156ad804b6c7f6423f6e576c3bdc62301`，用于公开发行索引。没有推送本地新增源码或私有快照。每个 Release 都明确说明：GitHub 自动生成的 Source code ZIP/tar.gz 是已有公开仓库快照，不是二进制的完整对应源码。每端另附 BUILD-INFO.json 与 SHA256SUMS。

主工作区及 Android / Windows 工作区的未提交维护工作保留。macOS / iOS 使用独立冻结快照构建。原工作区和用户词库未被删除或改写。Android 另存私有源码归档。

## 验证与限制

- macOS：arm64/x86_64 Release 构建通过，Apple Silicon 的隔离输入引擎、词库桥接、并击管理及引擎激活通过。自然码候选一致性断言有一项未通过：全拼 `tuan` 候选“图案”与自然码 `tr` 候选“团”存在分词差异；编码、预编辑和中文输出正常。保留失败记录，没有修改应用来绕过断言。最终 PKG 新装／升级及 Intel 实机运行未验收。
- Android：签名、16 KB 对齐、47 项核心测试和 Release lint（0 错误、12 警告）通过。此正式包已真机新装并验证 `nihao → 你好`。旧 `.debug` 应用与设置保留，测试后恢复原默认输入法。正式应用独立安装，设置、词库、凭据不会从开发版自动迁移；在线 AI 与其他机型未对这一签名包重测。
- Windows：x64/x86 各 16 项 CTest、10 项 EXE 载荷安全检查、安装结果接口回归与 100 个内置文件校验通过。真实最终 EXE 新装／升级／卸载与连续日用仍未验收。冷 Broker 原始字母泄漏、Edge 普通组字切框残留仍列为已知问题。
- iOS：已回读确认当前对外的 0.1.0（32）为 APPROVED / IN_BETA_TESTING，公开组链接开启，构建未过期。后续 1.0.0（34）已完成签名、上传与 Apple 处理，仍在外部测试审核中；不影响 build 32 继续对外提供。现有 App Store 0.1.0 审核未撤回。

## 公开说明更新

三个平台的发布页和 `RELEASE-NOTES.md` 已精简为下载、安装、使用说明与实际限制。macOS 发布页的 iOS 入口明确展示当前可安装的 0.1.0（32）。相关附件校验值已更新，回读与 SHA-256 核对通过，安装包内容及资产 ID 保持原值。此次文案更新前后的回执保存在 `.build/release-publish-1.0.0/public-copy-20261004/`。

## 私有构建与签名记录

- 本轮总目录：`.build/release-publish-1.0.0/`；各端线上回执为 `*-published.json`，iOS 回执为 `ios-provenance.json` 与 `ios-after-submit.json`。
- macOS 公证 ID：`727fb761-38a8-49e8-aa29-5617cdd062a2`，Accepted。临时签名钥匙串与复制的 API 私钥已移除，原有签名资料保留。
- Android 永久签名资料：`~/.config/rimes-release/android/`，说明见其中的 `README.txt`。本机恢复副本位于相邻 `backups/`；它不是异机备份，需要维护者另行妥善备份。私钥与密码不在仓库或 Release 附件中。

## 安装包 SHA-256

- macOS `RIMES-1.0.0.pkg`：`50c8b8b3603f59e3d5964c3e9805d2f0bc641439bd6706307becaefcf2606507`
- Android `RIMES-Android-1.0.0.apk`：`fd969ab393e239d947a9fb5bc3e33d6ba11cf2e1dde1c810126dfd3181897070`
- Windows `RIMES-Windows-1.0.0-Setup.exe`：`37c81abd8a36aa987f549a179ea29f8fd21faa9c1d32016a4fdd2769a83f5f1e`

公开二进制下载回读：macOS、Android、Windows 均 HTTP 200，无需登录；所有线上附件均与本地 SHA-256 一致。三个发行标签均指向既有公开提交，公开 main 和本地 HEAD 均未改变。

`v1.0.0` 自动触发的旧源码构建已结束（run 37136970463，cancelled）；本次发布依据上述本地签名、公证产物和验收记录，不把这轮旧源码 CI 当作实际二进制的构建证明。
