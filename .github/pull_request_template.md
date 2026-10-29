## 变更结果

<!-- 用一两句话说明用户可见结果。 -->

## 风险与边界

<!-- 焦点、secure input、Spaces、输入源身份、用户数据、插件权限或发布格式是否受影响？ -->

## 发布影响

<!-- 合并后是否需要发布预览版？一个 PR 只做一件事；提交信息使用 Conventional Commits（feat/fix/perf/refactor/docs/test/build/ci/chore/style/revert），发布说明由它们生成。见 docs/release/README.md。 -->

- [ ] 提交信息符合 Conventional Commits（CI「Release tooling」会检查）
- [ ] 如上次发布后 CI 提示 CHANGELOG 落后，已运行 `python3 scripts/release/release_tool.py changelog --write`

## 验证

- [ ] `swift build -c debug`
- [ ] 相关 smoke 已通过
- [ ] `python3 -B scripts/sync-buffer-plugin-catalog.py --check`
- [ ] `python3 scripts/lint-log-privacy.py`
- [ ] 已说明真实宿主/安装验证，或明确标注尚未验证
- [ ] 文档、README 插件版本表与 Release 契约已同步（如适用）
- [ ] 没有提交用户正文、剪贴板内容、密钥、token 或敏感路径

## 真实宿主回归

<!-- 填写 Codex / Notes / Obsidian / Chrome、输入方案、Buffer 状态、Spaces/显示器与结果。 -->
