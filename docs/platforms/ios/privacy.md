# iOS privacy disclosure

Publisher: Haiyang Cui. The website's bilingual [privacy page](../../ios/privacy/index.html)
and [support page](../../ios/support/index.html) provide the public disclosure and
contact information. Do not include credentials or private typing in public issues.

## 中文

本地剪贴板仅在你主动点击键盘内“收录当前文字”时，通过系统粘贴授权读取文字；
读取当前剪贴板需要完全访问，不后台监听。已收录的文字最多 40 条、总计 128 KiB、
单条 16 KiB，保存在键盘私有目录，排除备份，不上传或同步。你可逐条删除或清空。
点击历史条目只加入 Buffer，不自动上屏或运行插件。历史与会话结束即清除的 Buffer
草稿分别管理，已保存的本地历史无需完全访问即可使用。

犀术IME（RIMES）的普通输入在设备本地运行。学习词频只保存在当前设备，不提供账户或云同步。
我们不收集输入正文日志、广告标识符或使用遥测。Buffer 草稿保存在键盘会话内存中，
不写入文件；离开键盘会话时清除。
统计图片在本机生成，只含汇总统计，不含输入正文。主动保存图片时才申请相册添加权限，
不读取已有照片。App 保留最新一张主动保存的 PNG，不参与备份，可在卡片页面删除；
相册中的副本在照片 App 中管理。键盘图片导出需要完全访问，不会自动发送到聊天。
联想学习记录保存在键盘私有目录，不共享给主 App，也不参与备份。可在主 App 的“数据管理”
中确认清除，下次打开 RIMES 键盘时生效；只删除联想学习记录，保留输入方案、词库和键位配置。

AI 功能可选。你配置自己的 HTTPS 服务地址、模型和 API Key，并在明确同意接收方后
主动提交当前 Buffer 文本。请求包含这段文本、所选操作的指令、模型 ID 和用于向该
服务认证的 Key。不会自动发送宿主文档全文、剪贴板或后台打字记录。服务方会处理
请求并可能将它关联到其 API 账户；保留、训练和删除政策取决于你选用的服务，RIMES
不能替该服务承诺。请仅配置你信任的服务。

API Key 保存在本设备 Keychain，不包含在配置导出和备份中。请求不跟随重定向。
你可以删除服务配置及其 Key、撤销发送许可，或关闭键盘完全访问。关闭完全访问后，
普通输入及 Buffer 仍可用。卸载应用不会保证系统删除 Keychain 中的项目，因此建议
先在 AI 服务列表中删除配置，必要时到服务方撤销 API Key。

## English

Local clipboard reads text only when you explicitly tap Collect current text in
the keyboard, using the system paste control and Full Access. It never monitors
the clipboard. Up to 40 entries, 128 KiB total and 16 KiB each remain in private
keyboard storage, excluded from backup, never uploaded or synced. Delete entries
or clear the list. Tapping a record only adds it to Buffer, without automatic
insertion or plugin execution. Saved history is separate from ephemeral Buffer
drafts and remains usable without Full Access.

Ordinary typing runs locally. Learned word frequencies and learned next-word
associations stay in the keyboard on this device (never shared with the app, never
synced, excluded from backup). To clear learned associations, confirm the action
under Data management in the app; the keyboard applies it on its next presentation,
keeping input schemes, dictionaries and layouts. There
are no accounts, cloud sync, advertising identifiers, text logs or usage telemetry.
Buffer drafts stay in keyboard-session memory and are cleared when the session ends.
Stats images are generated on-device from aggregates, without typed text. An explicit
save requests add-only Photos authorization; existing photos are never read. The app
keeps the latest explicitly saved PNG, excluded from backup and deletable on its card
page. Photos copies are managed in Photos. Keyboard image export needs Full Access;
images are never automatically sent to a chat.

AI is optional. You configure an HTTPS provider, model and API key. After consenting
to the recipient, you explicitly submit the current Buffer text. The request includes
that text, the selected operation's instruction, model ID and authentication key.
Host documents, clipboard contents and background keystrokes are not sent automatically.
The provider processes the request and may link it to its API account. Retention,
training and deletion depend on that provider's policy; RIMES cannot promise them on
the provider's behalf. Configure only providers you trust.

Keys stay in this device's Keychain, outside configuration exports and backups.
Requests do not follow redirects. You can delete a provider and its key, withdraw
sending consent, or disable Full Access while continuing offline typing and Buffer.
Uninstalling does not guarantee Keychain deletion; delete configured providers first
and revoke the key at the provider when needed.

## Packaged privacy declarations

The [privacy manifest](../../../platforms/ios/Resources/PrivacyInfo.xcprivacy)
declares **Other User Content** and **User ID**, used for app functionality,
linked to the user and not used for tracking. These declarations cover optional
BYOK requests: selected text is transmitted, and the provider credential identifies
an API account. Ordinary local typing does not create a collection event; optional
AI must not be described as keeping all data on the device.

## Local translation and preferences

The keyboard stores its last selected input scheme, haptics preference and language
pair in its own preferences container. Buffer text and plugin results remain in
memory and are cleared when the keyboard session ends. These preferences do not
require Full Access or contain API keys or typed text.

Apple translation uses downloaded on-device language models through Translation.
The containing app offers the system model-download flow, which can require a
network connection. The keyboard does not fall back to an external AI endpoint
when local translation is unavailable. Existing explicitly selected AI providers
retain their separate recipient consent and Keychain behavior.
