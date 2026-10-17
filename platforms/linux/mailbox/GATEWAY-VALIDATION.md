# Linux 本地 MCP / HTTP 网关验证

对应 #170。网关在 Fcitx 宿主的独立工作线程运行；控制回调只更新内存，网络、JSON 和持久化不在输入事件线程执行。接收始终产生 Mailbox 待审核消息；加入 Buffer/忽略沿用 #169 的显式操作、修订校验与 `commitString` 链路。

## 共享契约

`gateway-linux-fixtures` 从实际 macOS `LocalGateway`、`InboundBus` 导出协议版本、四个工具的 schema、Origin 样例和大小边界。`gateway-settings-render` 用真实 AppKit 组件导出四种客户端的配置字符串、说明和 2× 截图；以易失偏好及隔离用户目录运行，不启用监听器或读取真实令牌。

`verify_gateway_contract.py` 校验四个原生源文件摘要，生成 Linux 构建所用只读契约。Linux 使用相同工具声明与客户端说明，测试逐字比较 JSON/TOML、环境变量和终端命令的脱敏配置。原生源变化后，构建要求重新导出并审查。

```sh
# macOS：仓库根目录，使用隔离构建出的程序
.build/out/Products/Debug/RimeBuffer gateway-linux-fixtures /tmp/native-gateway.json
RIMEBUFFER_APPEARANCE_MODE=night .build/out/Products/Debug/RimeBuffer \
  gateway-settings-render /tmp/native-gateway-settings
python3 platforms/linux/mailbox/verify_gateway_contract.py \
  --import-fixture /tmp/native-gateway.json \
  --import-settings /tmp/native-gateway-settings/settings-contract.json

# Linux：实际 GTK 页面，必须使用隔离目录
RIMES_USER_DIR=/tmp/linux-gateway-settings xvfb-run -a \
  platforms/linux/ime/build/runtime/rimes-settings --render-gateway /tmp/linux-gateway-render
```

网络格式参照 MCP 官方 [2025-06-18 HTTP transport](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports) 和 [tools](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)。保持 macOS 已支持的三个协议版本，不借此次移植扩大到 SSE 读取、远程监听或 SSH 客户端。

## 行为与错误路径

- `mailbox-gateway`：真实分段/连续 TCP 请求，严格 CRLF 与 Content-Length，重复头、Transfer-Encoding、超限、深层/重复键/无效 UTF-8 JSON 拒绝。验证 Host/Origin、令牌、版本协商、通知不执行工具、四个工具和来源过滤。
- 去重：HTTP 保存的请求摘要可跨重启复用；同一键不同正文冲突。MCP 流开始/追加重试不重复执行，去重缓存满时拒绝新身份，不淘汰仍有效流的重试信息；过期后释放容量。
- 流与容量：单条字符数、待审核数量、在途流数量、过期、跨令牌访问和保护态取消。保存前再次取得令牌、当前配置及实际插件授权的锁；配置或插件通知尚未到达时，磁盘撤权也阻止新保存。取消的流不发布部分正文。
- `rimes-gateway-fcitx-e2e`：加载真实 `rimes.so` 和 testfrontend，未启动 Mailbox 窗口即收到待审核文本。慢连接期间普通中文通过 Fcitx `commitString` 上屏；密码能力、真实私有 D-Bus 锁屏信号、配置文件监视和插件停用均关闭监听，恢复后不重放旧内容。该测试进入现有 `e2e.sh`，不只是模拟 Gateway 类。
- GTK 设置测试：四种预览、开关与重启配置、预览不创建令牌、复制真实配置的敏感标记、随后用户剪贴板不被取消操作清除。凭据不出现在普通预览或状态输出中。
- 原生数据往返：实际 TCP 产生的两条公开消息交给 macOS `MailboxStore` 重读，核对来源、正文、时间和待审核状态。Linux 去重字段作为 schema-v1 扩展保留，原生忽略未知字段。

## 已记录环境与结果

2026-10-18，本机隔离 macOS debug 编译通过；Ubuntu 24.04 arm64、GTK 3.24.41、Fcitx 5.1.7、锁定 Rime/Lua 引擎、Xvfb / 私有 D-Bus。

- 初次整树 CTest：70/70，77.21 秒；首次全新 deb 构建中的 CTest：70/70，78.34 秒。
- 安装脚本与版本测试：17/17；首次 deb 的安装、重装、移除通过，17 个用户数据哨兵逐项一致，包含网关令牌、收件、独立 Capsule 附件、词库和学习记录。
- 实际 Fcitx 网关集成测试及原生数据往返通过。最终集成拒绝空 MCP 请求 ID/缺少方法，并合入 Vault 锁忙回归修复。

最终候选包重新从源码构建：70/70 CTest，79.44 秒；真实 Fcitx 网关测试再次通过，最终 deb 安装/重装/移除及 17 个用户数据哨兵检查通过。arm64 包 `fcitx5-rimes_1.1.5_arm64.deb` SHA-256：`e0bb3ffb59fc987508aeea4c78d4694be9e09ec68bbd8045ba399736f6d6ff0d`。这是未发布的本地候选包。

## 视觉和交付门槛

[16 组双端实际截图、并排图和差异](../../../validation/linux-mailbox-gateway-20261018/README.md)：四种客户端 × 启用/关闭 × 墨竹/翡翠，980×680、2×。650/626 宽布局、46 高设置行、分段均分、图标底板、配置框与补充命令区域已校准；字体度量、图标、边框和数像素的纵向差异仍为 `requires_review`。

实际 GNOME/KDE 会话、各 Wayland 剪贴板接口、端口冲突提示及更多主题/缩放/悬停/焦点状态继续归入 #179。Xvfb 和私有 D-Bus 不替代真实桌面验收。文档说明来源名称是自报标签；真正撤销既有授权使用令牌轮换。

deb 保持项目 `VERSION`，随同一个根项目 `v*` Release 流程发布。此次验证只产生未发布构建，不创建 tag 或 Release；卸载保留 `gateway/`、`configuration/`、Mailbox、词库及学习数据。
