# RIMES 未签名预览版安装说明

> 预览版（tag 形如 `vX.Y.Z-preview.N`）是在首个完整签名、公证的正式版成功发布前提供的公开测试版。
> 它不是正式版：PKG 没有 Developer ID 签名、没有经过 Apple 公证，Apple 无法验证开发者
> 身份或确认这份软件通过了公证检查。

Apple 明确提醒，运行未经签名和公证的软件可能危及电脑、个人信息和隐私。请先阅读
[《在 Mac 上安全地打开 App》](https://support.apple.com/zh-cn/102445)，只在你理解并接受风险时继续。

## 一、只从官方 Release 下载

唯一允许的下载位置是 [scholay/xsime 的 GitHub Releases](https://github.com/scholay/xsime/releases)
中标为 **Pre-release** 的 `vX.Y.Z-preview.N`，通常选择最新的一个。

不要从网盘、群文件、论坛附件、短链接或第三方镜像下载，也不要安装别人重新打包的副本。
资产文件名应为 `RIMES-X.Y.Z-preview.N.pkg`，与 Release 标题中的版本号一致。下文用
`RIMES-0.5.0-preview.2.pkg` 举例，请替换成你下载的实际文件名。

下载后，在“终端”中运行：

```bash
cd ~/Downloads
shasum -a 256 RIMES-0.5.0-preview.2.pkg
```

把输出的 64 位 SHA-256 与官方 Release 中 `SHA256SUMS` 对应行公布的值**逐字核对**。
不一致就立即停止，删除文件并从官方 Release 重新下载。SHA-256 只能证明下载文件与发布者
公布的字节一致，不能替代 Developer ID 签名或 Apple 公证。

也可以把 `RIMES-0.5.0-preview.2.pkg` 和 `SHA256SUMS` 放在同一目录，只校验安装包对应的一行：

```bash
grep -E '  RIMES-0\.5\.0-preview\.2\.pkg$' SHA256SUMS | shasum -a 256 -c -
```

当前 `SHA256SUMS` 只列出同一次受控工作流生成的公开安装包；它不包含内部 handoff 或短期
Actions Artifact。

## 二、按 macOS 的单项例外流程安装

1. 双击下载的 `RIMES-X.Y.Z-preview.N.pkg`。必须先做这一步，让 macOS 记录本次拦截；不要先运行任何关闭安全
   功能的终端命令。
2. 出现“无法验证开发者”或“Apple 无法检查是否包含恶意软件”一类提示后，打开“系统设置”。
3. 进入“隐私与安全性”，向下滚动，在与 RIMES 对应的提示旁点“仍要打开”。
4. 按系统要求认证。警告再次出现时，确认文件来源和 SHA-256 都正确，再点“打开”。
5. macOS Installer 打开后按向导安装；安装到 `/Library/Input Methods` 需要管理员授权。

“仍要打开”只为这一个已拦截项目建立例外，不等于关闭整个系统的 Gatekeeper。上述顺序来自
[Apple 官方支持文档](https://support.apple.com/zh-cn/102445)。

## 三、安装后启用输入法

安装器会尝试为当前图形登录用户注册、启用并切换到“RIMES”。如果输入法菜单或“系统设置 →
键盘 → 输入法”没有立即出现 RIMES，请先**注销当前 macOS 账户并重新登录**，再到输入法设置中
确认或添加 RIMES。不要手动结束 `TextInputMenuAgent`、`imklaunchagent` 或其他 Apple 服务。

## 四、这些情况必须停止

- macOS 提示软件“已损坏”或“将损坏你的电脑”：不要点开、不要寻找绕过命令；删除下载文件，
  并到官方 Release/Issue 核对是否有新通知。
- SHA-256 与官方 Release 不一致：不要安装。
- “仍要打开”不存在或被禁用：公司、学校等组织管理的 Mac 可能通过 MDM 禁止例外。请联系
  管理员，或等待正式签名版；不要试图规避组织策略。

无论遇到什么提示，都不要执行以下做法：

```text
sudo spctl --master-disable
xattr -d com.apple.quarantine ...
xattr -cr ...
```

这些命令会绕开或削弱 macOS 的安全边界，不是 RIMES 支持的安装方式。

## 五、升级到新的预览版

预览版不进入应用内自动更新。新预览版发布后，按第一、二节重新下载、校验并安装新的 PKG 即可；
安装器会原地替换 `/Library/Input Methods/RIMES.app`，设置与词库保留在 `~/Library/RIMES`。
每个预览版 Release 页面都列出了自上一版本以来的变更。

**从 `v0.5.0-preview.1` 升级时**：该版本安装的是改名前的 `ETInput.app`，之后的版本改为 `RIMES.app`，
输入法 id 也随之更换。安装新 PKG 时安装器会自动移除旧的 `ETInput.app`；新版首次启动时，把
`~/Library/RimeBuffer` 中的设置、词库和并击方案一次性复制到 `~/Library/RIMES`，旧目录保留不删。
安装后到“系统设置 → 键盘 → 输入法”确认只有一个 RIMES；若菜单里仍显示旧项，注销并重新登录一次。

## 六、将来升级正式版

预览版是 GitHub **Pre-release**，且不满足 RIMES 更新器的 Developer ID / 公证校验，因此应用内自动
更新不可用。完整的 Developer ID、Installer、公证与受保护发布链路就绪后，会用新的、更高版本号发布签名并
公证的正式 PKG，不会替换任何既有预览版的字节。届时请从官方 GitHub Release 手动下载安装一次；之后的
正式版即可应用内更新。
不要继续分发或安装旧的未签名预览包。
