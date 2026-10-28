# Linux deb 生命周期与同版本发布

Debian/Ubuntu 用户优先使用项目 `v*` Release 附带的 `fcitx5-rimes_<version>_<arch>.deb` 与 `.sha256`。完整安装、升级、源码迁移和卸载操作见 [安装说明](../../../platforms/linux/ime/README.md#install-and-upgrade-debian--ubuntu-recommended)。编译后直接运行 `install.sh` 适合开发；删除源码或构建目录不能清除已复制的安装文件，需用相同前缀的 `uninstall.sh`。deb 交由 `apt remove fcitx5-rimes` 卸载。

源码卸载按清单及摘要处理应用文件，包卸载由 dpkg 管理。两者保留用户词库、学习、导入方案、Capsule 内容/历史/密码、Mailbox 会话和附件、配置与凭据、统计和练习记录，CLI 自己的认证也保留。

## 持续发布验收

`linux-deb.yml` 在一次性 Ubuntu 24.04 amd64 runner 构建项目同一版本/提交，完整执行原生回归和包生命周期：

1. 全新安装本次 deb，检查安装后的 ELF 依赖、固定词典/插件资源、设置页面和 Mailbox 实际进程，再卸载。
2. 从项目已经公开的 `v*` Release 选择低于新版本、架构相同且包含摘要文件的最高 Debian 版本。下载后校验 SHA-256、包名、版本、架构，再安装旧包并升级到本次包。
3. 升级后再次运行安装后的原生检查，重装本次包，最终卸载并检查所有本次包文件已移除。
4. 每步核对临时公开样例中的用户/CLI 文件内容、目录布局、权限和所有者。包括用户自定的非秘密文件权限；覆盖项以当前生命周期测试为准。

没有已发布的同架构旧包时，明确用当前载荷生成严格较低版本的夹具，并加入只属于旧包的文件，验证升级会退役旧文件。此结果只能证明 dpkg 升级与数据保留，不能冒充历史应用数据迁移。API、下载、摘要或包身份错误直接失败，不退回夹具。

预览版 `vX.Y.Z-preview.N` 使用 Debian 版本 `X.Y.Z~preview.N`；正式版跟随根 `VERSION`。所有 `platforms/linux/**` 变化触发发布演练。公开 Release 等待 Linux 包验证，然后将同一已验证字节的 deb 和摘要附加到同名 `v*` Release，并下载读回；重试不覆盖不同内容。PR、手动与夜间演练只保留短期 CI 产物。

真实宿主和视觉行为另按 [桌面验收](acceptance.md) 检查。流程配置、夹具或某次 CI 成功不代表所有桌面环境或公开资产已经验收。
