# XSIME Linux

Linux 现在有两层，互相独立：

1. **[Fcitx5 XSIME 输入法](ime/README.md)** — C++ addon，链接 librime，部署审核后的
   XSIME `rime-data`，用 Fcitx5 的 preedit/候选 UI 上屏。**[Buffer 工作台](buffer/README.md)**
   和 **[Capsule 底栏](capsule/README.md)** 已接到同一 addon。Buffer 把 Rime 提交分块
   暂存后再上屏；Capsule 用本机 Markdown 笔记库，经 `commitString` 插入选中笔记。
   规格见 [buffer/SPEC.md](buffer/SPEC.md) 与 [capsule/SPEC.md](capsule/SPEC.md)，
   真机清单见各自 `MANUAL-TEST.md` 与 [ime/MANUAL-TEST.md](ime/MANUAL-TEST.md)。
   **[Mailbox](mailbox/README.md)** 已有独立 GTK 窗口、三模块导航和兼容会话存储；AI 对话、PTY 与收件流转按后续任务继续接入。
2. **下面的 Data / Input-Schemes Preview** — 仍把同一批 57 个审核文件装进用户已有的
   Fcitx5 Rime 或 IBus Rime。数据包保持原样，不依赖上面的 addon。

IBus 自己的 XSIME 引擎尚未开始；需要什么见 IME README。

**安装完整 Linux 输入法：Debian／Ubuntu 推荐使用项目 `v*` Release 附带的 deb。**
用 `apt install ./fcitx5-rimes_<版本>_<架构>.deb` 安装或升级，
用 `apt remove fcitx5-rimes` 卸载并保留用户词库与学习记录。
源码安装、完整输入法卸载脚本及发布维护要求见
[完整输入法安装与卸载说明](ime/README.md#install-and-upgrade-debian--ubuntu-recommended)。
下文的安装／卸载脚本仅适用于数据预览包。

---

# XSIME Linux Data / Input-Schemes Preview

这是一个面向 Linux 的 **Rime 数据与输入方案预览包**，首发目标是
[Fcitx5 Rime](https://github.com/fcitx/fcitx5-rime)，同时兼容
[IBus Rime](https://github.com/rime/ibus-rime)。它把仓库审核后的 `rime-data` 最小闭包
安装到所选前端的用户数据目录，默认提供雾凇全拼、自然码双拼、小鹤双拼、五笔 86
和英文五个核心方案；`my_combo` 飞耀方案随包提供，但作为默认关闭的可选并击方案。

这不是完整的 Linux 版 XSIME 应用。独立的 Fcitx5 addon 与 Buffer 在 [ime/](ime/README.md)
和 [buffer/](buffer/README.md)。
数据包本身不包含 Buffer 工作台、Capsule 底栏、AI/翻译、OCR、插件 UI、自绘候选窗、焦点租约或设置窗口；
走数据包路径时，候选显示、上屏和部署仍由用户已经安装的 Fcitx5 Rime 或 IBus Rime 负责。
数据包中的 `my_combo` 可把同一批次按键交给 Rime chord composer，并包含单键 `v`
修复；XSIME macOS 前端实现的跨批分离击键配对不在本预览中。

## 前置条件

- 已安装并能正常使用的 Fcitx5 Rime（推荐）或 IBus Rime。
- 前端所用的 librime 必须带 Lua 模块；本包的核心和可选方案包含 Lua processor、translator
  和 filter。不同发行版可能把它命名为 `librime-lua`、`librime-plugin-lua` 或随
  `fcitx5-rime`/`ibus-rime` 一起提供，请以发行版的软件包说明为准。
- 运行时还须提供 Rime/OpenCC 的标准 `opencc/s2t.json` 及其字典；审核工具把它登记为
  外部运行时依赖，不会在用户目录重复打包。通常由发行版的 `rime-data`/OpenCC 包提供。
- Bash、`tar`，以及 `sha256sum`、`shasum` 或 `openssl` 中的任意一个。
  文件事务还需要支持 `ln -T` 的 GNU/BusyBox `ln`；若系统的 `ln` 没有该选项，脚本可用
  Python 3 的 `os.link` 作为 exact-destination fallback。两者都不可用时安装会安全停止。

Rime 官方资料列出的 Linux 用户目录是：

| 前端 | 用户目录 |
|---|---|
| Fcitx5 Rime | `${XDG_DATA_HOME:-$HOME/.local/share}/fcitx5/rime` |
| Fcitx5 Flatpak | `$HOME/.var/app/org.fcitx.Fcitx5/data/fcitx5/rime` |
| IBus Rime | `${XDG_CONFIG_HOME:-$HOME/.config}/ibus/rime` |

Fcitx5 的标准路径见其官方
[迁移说明](https://fcitx-im.org/wiki/Upgrade_from_Fcitx_4)，IBus 与 Fcitx5 的
Rime 用户目录也列在 Rime 官方
[数据文件说明](https://github.com/rime/home/wiki/RimeWithSchemata)中。

## 安装

从发布归档解压后，在任意工作目录执行：

```bash
/absolute/path/RIMES-Linux-Data-Preview-*/scripts/install.sh --frontend fcitx5
```

IBus：

```bash
/absolute/path/RIMES-Linux-Data-Preview-*/scripts/install.sh --frontend ibus
```

也可以不传 `--frontend`，让脚本根据 XDG 目录、输入法环境变量和已安装命令自动选择。
如果同时发现 Fcitx5 与 IBus，脚本会停止并要求显式选择，不会猜测。

CI 或非标准布局可完全跳过前端探测：

```bash
tmp_root=$(mktemp -d)
/absolute/path/scripts/install.sh --dest "$tmp_root/rime"
/absolute/path/scripts/verify.sh --dest "$tmp_root/rime"
/absolute/path/scripts/uninstall.sh --dest "$tmp_root/rime"
```

`--dest` 必须是绝对路径且以 `/rime` 结尾。它不要求 Fcitx5/IBus 已安装或正在运行，
也不会重启输入法。

### 部署

安装默认只提交文件事务，不部署、不重启：

- Fcitx5：推荐从 Fcitx5 的 Rime 状态/菜单选择 **Deploy**。也可以在安装命令加
  `--deploy`，脚本会调用 `fcitx5-remote -r` 让前端重新读取变更。
- IBus：推荐使用状态栏的 **Deploy**；`--deploy` 按 Rime 官方
  [定制指南](https://github.com/rime/home/wiki/CustomizationGuide)更新用户目录时间并执行
  `ibus restart`。
- Fcitx5 Flatpak：脚本故意不自动终止沙箱进程，请在 UI 中执行 Deploy/重启。

首次部署词典可能需要一段时间。没有 `--deploy` 时，脚本只打印下一步，不会打断当前
输入会话。

## 验证与卸载

```bash
./scripts/verify.sh --frontend fcitx5
./scripts/uninstall.sh --frontend fcitx5
```

`verify.sh` 默认仅验证安装 manifest 和每个文件的 SHA-256，不依赖运行中的输入法。
`--check-runtime` 会额外检查前端命令是否存在，但仍不能替代真实编译和打字测试。

卸载只删除由本安装器记录且哈希仍相同的文件。安装后被用户编辑过的文件会保留，脚本
在删除任何文件前返回非零并留下完整管理状态，用户可先移动或恢复文件再重试。没有
`--force` 删除模式。

## 安全边界

- 安装前完整检查冲突；任何目标文件已存在都会在复制前终止，默认永不覆盖用户配置。
- Payload 明确拒绝 `build/`、`userdb`、`*.userdb`、生成的 `*.bin`、日志和锁文件。
- 不读取、复制或删除学习词库，不运行 `rsync --delete`，不清空 Rime 用户目录。
- 每个文件先复制到同目录临时文件、校验 SHA-256，再用 exact-destination、no-replace
  hard link 原子提交；中途失败按 journal 回滚仍由本事务持有的文件。
- 事务锁只串行化本包的安装、验证和卸载脚本；事务运行期间请勿另行部署，或从其他进程
  编辑受管方案文件。脚本会尽量检测外部写入并保留替换内容，但该锁不是系统级文件隔离。
- 拒绝目标目录或目标子路径中的符号链接，避免把写入或卸载操作引向其他目录。
- `--deploy` 是唯一会请求前端 reload/restart 的入口，默认关闭。

## 构建发布包

在源码仓库中从任意目录运行：

```bash
/absolute/path/platforms/linux/scripts/package.sh --output-dir /tmp/rimes-release
```

输出包括 `.tar.gz` 和对应的 `.sha256`。打包脚本必须先调用统一的
`scripts/platform-preview/preview.py stage`，只收录 policy 审核通过的 57 文件最小
依赖闭包；`rime_ai.example.json` 和旧 AI Lua 明确排除。归档内另有覆盖每个 payload
文件的 `data/PAYLOAD-MANIFEST.tsv`，安装前会自动校验。找不到 Python、统一 staging
工具或 policy 校验失败时，构建会 fail-closed，不会退回复制整个 `rime-data`。

归档同时保留项目 `LICENSE`、`THIRD_PARTY_NOTICES.md` 和经过 policy 校验的
`rime-data/licenses/`。XSIME 自有脚本采用 Apache-2.0，历史 MIT 授权范围见根目录 `LICENSING.md`；后续发行应附带 `LICENSE`、`NOTICE` 及适用的保留许可。雾凇及衍生数据按 GPL-3.0-only，
easy-en 词典按 LGPL-3.0，`lua/search.lua` 保留 CC BY-SA 4.0 署名。精确来源边界见包内
第三方声明和数据目录中的 source notice。

在源码仓库中运行本机文件事务测试：

```bash
./tests/smoke.sh
```

该测试只使用 `mktemp` 创建的 HOME/目标目录，覆盖安装、校验、卸载、冲突拒绝、
`build`/userdb 哨兵保护和发布归档往返，不调用 Fcitx5 或 IBus。
