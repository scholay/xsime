# XSIME 命名与兼容规范

| 用途 | 名称 |
| --- | --- |
| 简体中文显示名 | 犀术IME |
| 繁体中文显示名 | 犀術IME |
| 英文及其他语言默认显示名 | XSIME |
| 项目与架构名称 | XSIME |
| 仓库 | `scholay/xsime` |
| 官网 | `https://xsime.com` |

品牌标语为“心有灵犀，落笔见术”（繁体“心有靈犀，落筆見術”）。标语中的“灵犀”保留原文。Rime / librime 是上游引擎名称，保持原名、版权和来源说明。RIMES、灵犀输入法、Lingxi IME 只用于兼容标识或历史迁移说明。

## 展示名称与兼容身份分开

品牌调整不能改变以下内容：

- macOS Bundle ID、输入源 ID、IMK 控制器类、`RIMES.app` 和 `RIMES` 可执行文件名。
- iOS Bundle ID、App Groups、Keychain Groups；Android applicationId 与长期签名。
- Windows CLSID、注册表键、协议和安装所有权标记；Linux 包名和 addon ID。
- 用户数据路径、偏好键、凭据、词库、方案 ID（包括 `lingxi_wubi`）与插件 ID。
- 已发布的资产名、版本、摘要、Release 与原始提交。

macOS 连接名使用 `com.scholay.inputmethod.isaac_Connection`，遵循既有 Bundle ID 加 `_Connection` 的约定；以 [Info.plist](../../Info.plist) 和运行时入口共同校验。

不要全仓无差别替换 `rimes`、`RIMES` 或 `lingxi`。固定版本的官方插件需在其源码仓库修改并重新锁定，不能直接改本体中的生成副本。第三方名称和词典中的自然语言词条保持原义。

## 仓库与更新兼容

更新器使用固定仓库身份，并对官方新旧仓库 URL、包身份、版本和签名进行校验。旧版本内置更新器可能拒绝仓库重定向；受影响用户可从官方 Release 手动升级一次，保留 `~/Library/RIMES` 等用户目录。

不重新建立 `scholay/rimes` 占用旧地址，不因品牌调整改变既有安装包身份。官网迁移仅调整产品入口；已发布的帮助与隐私页面路径、上游版权链接和维护者主页按各自用途保留。

商店、TestFlight 和备案名称按各渠道实际流程更新。源码中的展示名称不能作为商店已更新的依据，也不能改写旧发布记录中的事实。
