# Contributing to RIMES

感谢你帮助改进 RIMES。提交代码前，请先在最新 `main` 上复现问题，并确认没有把用户输入正文、
API 密钥、剪贴板内容或本机路径放进 Issue、日志和测试夹具。

统一分支、各平台独立构建和发布的日常约定见 [docs/development/maintenance.md](docs/development/maintenance.md)。
本地整合可以领先 `origin/main`；没有明确授权时，不推送、不创建发布 tag、不安装。

## 报告问题

- Bug 请使用 GitHub 的 Bug 表单，写清 macOS、RIMES 版本/commit、宿主应用、输入方案、
  Buffer/剪贴板状态、Spaces/显示器环境和最短复现步骤。
- 输入法行为必须区分“源码 smoke 通过”和“安装后的真实宿主验证”。候选窗、焦点、secure input、
  输入源切换和跨 Space 问题不能只凭单元测试宣布解决。
- 附日志前先脱敏。RIMES 的日志不得包含输入正文、候选正文、剪贴板内容、token 或密钥。
- 验收截图、对比图、重复截图和原始日志不提交到 Git；保留在本地或 CI 附件中。
  仓库保留验收结论、来源、哈希、结构化摘要和复现脚本，具体见
  [验收资料存放规则](validation/README.md)。不要用 `git add -f` 绕过附件忽略规则。

## 开发与验证

先阅读[文档索引](docs/README.md)、[系统架构](docs/architecture/system.md)和
[维护规则](docs/development/maintenance.md)。产品构建与自动测试在远端 CI 执行；
本地文档或静态检查不能替代平台构建和真实宿主验收。

提交前检查改动范围、相对链接和 `git diff --check`。按变更涉及的平台选择 CI，
记录确切提交、自动检查结果和仍需实际设备验证的场景。

说明优先补充现有专题；不要把临时排查过程、逐次构建流水和已完成的发布准备另存成文档。

## Pull Request

- 一个 PR 聚焦一个可审查的目标；说明风险、验证证据和仍未验证的真实宿主场景。
- 不要修改冻结的输入源身份、当前 `RIMES.app` 安装路径或 `Delivery.insert` 投递边界，除非同时提供
  明确迁移方案和回归矩阵。
- 新增/更新预置 Buffer 插件时，同步 `Catalog/buffer-plugins.json`、生成文件和 README 版本表。
- 不要从 fork、旧 tag 或本地重打包直接发布 Release。

## 贡献归属

人类维护者负责最终审查、合并和发布。AI 编码智能体实质参与设计、实现、测试或审查时，按
[`CONTRIBUTORS.md`](CONTRIBUTORS.md) 的规则使用已验证的公开身份添加 `Co-authored-by` trailer；
不要虚构身份，也不要仅为补署名改写已发布历史。
