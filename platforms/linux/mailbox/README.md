# Linux Mailbox

`rimes-mailbox --show` 打开独立 GTK 工作区。应用菜单的 **XSIME Mailbox**、已启用的 `Alt+Shift+M` 和 `rimes-launch mailbox` 复用同一用户目录的窗口。后两者经 Fcitx 快捷键宿主检查；Mailbox 自身监听桌面会话保护。关闭窗口或在非终端输入区按 `Ctrl+W` 隐藏窗口；终端获得焦点时 `Ctrl+W` 和 Escape 交给终端。`rimes-mailbox --quit` 退出进程并结束本次 Mailbox 的 PTY，保留已保存收件。

窗口包含终端、对话、收件三个官方模块入口、收件未读角标、列表搜索和筛选。支持查看已有对话/收件、重命名、归档/恢复和本地备注；备注只保存在本机。窗口正常大小为 1000×700，最小 680×500；窄窗口通过顶部侧栏按钮展开列表，点击正文外侧或 Escape 收起。显示并激活的收件才标记已读。窗口尺寸与最后模块自动保存；未发送草稿按模块/会话保留在当前进程，退出进程不持久化草稿，与当前 macOS 行为一致。

宿主与收件流转分别对应 [#168](https://github.com/scholay/xsime/issues/168)、[#169](https://github.com/scholay/xsime/issues/169)。本地 HTTP/MCP 来源接入对应 [#170](https://github.com/scholay/xsime/issues/170)，CLI 渠道在 #171/#172，AI 对话在 [#173](https://github.com/scholay/xsime/issues/173)；PTY 终端见 [#174](https://github.com/scholay/xsime/issues/174)。

## PTY 终端

打开 **终端 → 选择 Shell / Codex / Claude Code → 新建会话**，选择这个会话的工作目录。Shell 使用当前用户的 `$SHELL`（不可用时用 Bash）；Codex 和 Claude Code 需要先安装对应 CLI。终端是交互式 PTY，支持中文、ANSI、滚动回看、键盘控制和随窗口调整行列。`Ctrl+C` 发送终端中断；`Ctrl+Shift+C/V` 明确复制选区或粘贴剪贴板。

每个会话有自己的进程、工作目录和 UUID。切页、隐藏窗口、暂时停用插件或锁屏保留进程；锁屏和插件不可用时不能操作终端或收取文件。**结束当前终端进程**会终止该 PTY 会话中的进程组，必要时强制结束；退出 Mailbox 也会清理它持有的会话。主动脱离 PTY、另建系统会话的后台服务由用户自行管理。应用重启后不会伪造恢复旧进程，已收取的成果仍在收件箱。

交互式 CLI 使用用户自己的 HOME 和 CLI 登录/授权流程，不沿用 Buffer 后台任务的只读文本参数；Codex 的普通终端登录与后台受管 Codex HOME 分开。终端启动不会读取 Buffer 或自动把输出发到输入框。每次 Mailbox 运行最多保留 16 个终端会话，滚动缓冲最多 10,000 行。

右侧 **文件** 列出工作目录中最多 60 个普通文件，按自然顺序排序，不递归、不跟随链接、不显示隐藏文件；为避免大目录阻塞，最多扫描 10,000 项。也可以用右上角 **选择文件** 打开子目录中的成果。支持 UTF-8 文本（最多 1 MiB）和已安装解码器支持的 PNG/JPEG/GIF/WebP/BMP/TIFF/ICO 图片（原文件最多 24 MiB，单边最多 16,000 像素、总计最多 3,200 万像素）。图片独立解码为 PNG；损坏、特殊文件、链接、超限或超时会报错并保留原件。

预览后点 **收入收件箱**，保存当时预览的文字或图片副本，按会话 UUID 关联。文件名、终端标题、工作目录相同也不会混淆会话；不推断某个文件一定由 CLI 生成。切换会话、隐藏、锁屏或取消时，未提交的预览/收取作废。附件和消息一起提交，失败清理本次临时附件；重复点击不会重复提交。收件中的 **返回终端** 仅返回本次运行仍存在的会话。

## AI 对话

打开 **对话 → 新对话**，从输入框下方选择 Codex CLI、Claude Code 或已经配置模型的 API Provider，再输入消息并点 **发送**。CLI 使用适配器验证过的默认模型；API 会保存选中的 Provider、模型和配置修订。已有会话始终使用保存的来源；切换全局默认连接器/Provider 不会改变它。修改、删除 Provider 或更换密钥后，旧路由不再自动继续，需要新建会话重新选择。

`Enter` 发送，`Shift+Enter` 换行；输入法组字的 Return 先交给输入法。生成中仍可写下一条草稿，但当前会话完成前不能重复发送。部分回复只在内存显示，完整成功的最终回复才写入历史；多块回复用空行分隔。**停止生成**保留已提交的问题并记录取消；失败后的 **重试**复用最后的问题，不重复添加一条。搜索、重命名、归档、恢复和独立草稿与收件工作区共用。

切页、隐藏窗口或 `Ctrl+W` 不取消后台对话；停用对话插件、进入桌面保护状态或退出 Mailbox 会取消。进程意外退出后，下次打开会把没有运行进程的任务标为中断；不会把未完成的流恢复成已完成的回复。已读、重命名和归档不会使正在提交的续聊失效；历史内容或来源发生变化时拒绝旧请求。

最多同时生成 4 个对话，每个请求默认 120 秒；提问最多 256 KiB，完整请求上下文最多 1 MiB。历史内容超过范围时请新建会话。续聊上下文只包含用户提问和 AI 正文，不包含本地备注或状态事件。CLI 未连接、路由失效、容量不足或提交失败时保留草稿。历史 API 会话没有保存的路由时不会猜测当前 Provider；旧 CLI 会话首次续聊会固定适配器默认模型。

对话不会自动读取 Buffer、自动上屏或执行工具。需要使用结果时，明确点 **加入 Buffer**，再在 Buffer 中编辑和发送。API 密钥保留在私有配置目录，CLI 凭据仍由各自渠道管理，不进入 Mailbox JSON。实际账号、宿主与跨模块行为按 [桌面验收](../../../docs/platforms/linux/acceptance.md#mailbox) 分别检查。

## 处理收件

- **复制**将消息正文写入桌面剪贴板。**保存图片**打开 PNG 保存对话框，取消不会写文件；覆盖现有文件需要在对话框明确确认。
- **存入 Capsule**独立保存文字笔记或 PNG；相同内容复用已有条目。PNG 位于 `capsule/assets/`，之后删除 Mailbox 原件仍可使用 Capsule 副本；删除 Capsule 条目也不删除原收件。
- **加入 Buffer**是明确接受这条外部文字的操作。Fcitx 宿主重新读取实际消息，核对来源、修订、模块授权、输入状态和连接后完整追加；不会替换已有选区或自动发送。打开 Buffer 继续编辑，再明确发送到目标输入框。容量不足、来源被修改、焦点变化、停用模块或关闭窗口时取消并保留原文。
- 待处理收件可以**忽略**；接受或忽略后保留原消息和处理状态，避免重复接受。单向来源的输入区只添加**本地备注**，不会回复来源。保存完成后切换窗口不会留下重复草稿，保存期间继续输入的新草稿会保留。
- **归档**可恢复；**删除会话**需要确认，删除该会话及不再被其他会话引用的原附件，保留其他草稿和 Capsule 副本。

接收过程中退出 Fcitx 的未完成记录，会在下次启动时自动恢复为待处理；若本次已暂存到 Buffer，清理收件状态会重试。Buffer 原文仍是当前进程的暂存内容，退出前请完成编辑或保存。

## 本地 MCP / HTTP 网关

Fcitx 完整版运行、收件模块启用且会话未受保护时，网关监听 `127.0.0.1:47700`。网关由输入法宿主持有，关闭 Mailbox 窗口不会关闭网关。外部文字保存为**待审核收件**，不会拉起 Buffer、获取输入目标或自动上屏；在 Mailbox 中明确选择加入 Buffer 或忽略。密码输入、锁屏、睡眠、停用收件模块或关闭网关会关闭现有连接并丢弃未结束的流，已经保存的消息保留。

打开 **设置 → 连接器 → 本地网关**，启用网关并选择 Cursor、Codex、Claude Code 或通用 MCP。配置预览始终脱敏，点按复制时才读取真实令牌。Codex 的配置使用 `RIMES_MCP_TOKEN` 环境变量，另有“复制环境变量”；Claude Code 另有可选终端注册命令。复制带有敏感内容排除标记，60 秒后只清理本次复制，不覆盖用户后来复制的内容。桌面未提供所需剪贴板接口时显示失败，不显示真实令牌。

HTTP `POST /v1/inbound` 接收 JSON：`text` 必填，`title`、`source` 和 `format` 可选；格式为 `plain`、`markdown` 或 `json`，原文和格式均保存。`GET /v1/health` 只返回无用户信息的健康状态；其他接口要求 `Authorization: Bearer <token>`。请求可以使用稳定的 `Idempotency-Key`，相同授权下重试相同内容复用原收件，不同内容复用同一键返回冲突。重装或服务重启仍能识别已保存收件的键；删除该会话或更换令牌后不再属于同一去重范围。

MCP 使用 `/mcp` 的 stateless HTTP POST JSON 模式，提供与当前 macOS 相同的 `buffer_push`、`buffer_stream_begin`、`buffer_stream_append`、`buffer_stream_end` 四个工具。流式内容只有在 end 时成为完整待审核收件；关窗不影响流，关闭网关、保护态或授权/配置变化会取消流。工具不读取现有 Buffer、收件或词库，不自动发送；未实现 SSE 读取或 SSH 客户端。

排查和撤销操作：

```sh
rimes-gateway --status
rimes-gateway --block-source 'http:my-agent'
rimes-gateway --unblock-source 'http:my-agent'
rimes-gateway --rotate-token
```

来源名称由发送端声明，只用于展示和过滤，不能证明发送者身份。MCP 名称取当前连接的 `initialize.clientInfo.name`，未初始化的连接显示 `mcp`。阻止名称后持有令牌的客户端仍能换名，因此撤销已有客户端权限应使用 `--rotate-token`，然后只向继续授权的客户端重新复制配置。命令和状态输出不包含令牌或正文。

边界：32 KiB 请求头、256 KiB 请求体、单条 20,000 个 Unicode 字符簇；最多 50 条待审核收件、50 个在途流，每个流最长 5 分钟，在途流的 UTF-8 内容合计不超过 1 MiB。未处理收件正文合计上限为 1,048,576 个字符簇。最多 16 个连接，每个连接最多 100 个请求；不完整请求 5 秒、空闲连接 30 秒关闭。共享令牌的请求速率为每秒 120 次，突发容量 120 次。MCP 最多保留 256 个成功请求的去重键，5 分钟内填满时拒绝新键，保留已有流的重试身份。

## 安装、更新与保留数据

Debian/Ubuntu 推荐从项目同一个 `v*` Release 安装完整版 deb；开发源码安装/卸载方法见 [IME 安装说明](../ime/README.md)。deb 和 `install.sh` 同时安装 `rimes-mailbox`、`rimes-gateway`、隔离的文件预览程序与桌面菜单入口。GTK VTE 终端运行库由 deb 依赖声明；源码依赖用 `platforms/linux/ime/scripts/deps.sh --install` 安装。卸载 deb 使用 `apt remove fcitx5-rimes`；源码安装使用相同 prefix 的 `uninstall.sh`。不要直接删除共享安装目录。

用户根目录为 `RIMES_USER_DIR` 或 `$XDG_DATA_HOME/rimes`（默认 `~/.local/share/rimes`），其下 `mailbox/` 包含：

- `mailbox.json`：兼容 macOS schema-v1 的会话、消息、来源、处理状态与 Foundation 日期。
- `images/<UUID>.png`：发布消息前先独立持久化的 PNG 附件。
- `window.json`：窗口大小和最后模块；`host/`、`window/`、`transfer-host/` 中的锁文件只用于进程与写入协调；JSON 中的 Linux 接收事务字段用于恢复中断，不改变 macOS 的 schema-v1。

用户根目录下的 `gateway/token` 保存私有授权令牌，`gateway/status.json` 保存不含正文或令牌的运行状态。配置中的开关、端口和来源过滤位于 `configuration/preferences.json`。安装、重装及卸载同样保留这些用户文件。

这些目录为 0700、文件为 0600。内容写入使用原子替换，写操作校验预期修订、模块授权与会话状态。关闭、停用模块、重装和卸载保留 Mailbox 数据，也保留用户词库和学习记录。数据损坏时显示错误并保留原文件，不以空文档覆盖；恢复备份前先退出 Mailbox。跨总线的第二个进程也不能夺取同一目录的主实例锁。

## macOS 组件同步

macOS 的生产 UI、模型与官方模块声明是基准。`MailboxUI` 共享命名尺寸常量，`mailbox-linux-fixtures` 用隔离目录/偏好导出真实 AppKit 截图、解析/筛选样例和主题色。Linux 通过 `verify_native_contract.py` 检查来源摘要，并消费相同尺寸、模块和颜色；修改基准后必须重新生成并审查，构建不会静默使用旧基准。

```sh
# macOS，仓库根目录，已完成隔离 debug 编译
TZ=Asia/Tokyo .build/out/Products/Debug/RimeBuffer mailbox-linux-fixtures /tmp/mailbox-native
python3 platforms/linux/mailbox/verify_native_contract.py --import-fixture /tmp/mailbox-native/native-mailbox.json
# Linux，使用真实 GTK 工作区导出同尺寸 2× 截图
TZ=Asia/Tokyo xvfb-run -a platforms/linux/ime/build/rimes-mailbox \
  --render-fixture platforms/linux/mailbox/tests/fixtures/native-mailbox.json \
  --render-dir /tmp/mailbox-linux
```

行为、视觉和真实服务分别按 [桌面验收](../../../docs/platforms/linux/acceptance.md#mailbox) 记录；功能测试通过不代表像素一致或真实 GNOME/KDE 桌面已验收。
