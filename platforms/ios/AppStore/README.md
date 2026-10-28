# App Store 资料

- [`metadata/`](metadata/)：英文、简体中文和繁体中文商店文案。
- [`release-notes/`](release-notes/)：按版本保存的三语更新说明，格式见该目录 README；发布脚本直接读取这些 JSON。
- [`review-notes.txt`](review-notes.txt)：审核人员使用的操作说明。
- [`submission-1.1.5/screenshots/`](submission-1.1.5/screenshots/)：已采用的商店截图及文件说明。
- [构建、签名与发布](../../../docs/platforms/ios/release.md)：CI、云签名归档和提交入口。
- [资源许可与发行依据](../../../docs/platforms/ios/resources.md)、[隐私说明](../../../docs/platforms/ios/privacy.md)：随包资源和数据处理说明。

发布前确认版本资料与实际构建一致。上传、审核和商店可用状态分别从 Apple 读回，不在此目录维护旧审核过程日志。
