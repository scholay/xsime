# Scholay Buffer 插件

插件卡片显示出品方：ChatGPT 为 OpenAI，Claude 为 Anthropic；Reference、Polisher、LaTeX 为 Scholay。其他内置插件显示 RIMES 官方。

1. 在「设置 → 插件 → 缓冲插件」找到 Reference，点击齿轮，在插件配置页填写 Minicod API Key。密钥保存在本机 `~/Library/RIMES/plugin-config/builtin.scholay/configuration.json`，文件权限为 `0600`；现有 Scholay 配置继续使用。
2. 在 Buffer 工具栏选择 Reference，再选择生成服务（ChatGPT、Claude 或 AI API）和引用格式（GB/T 7714、APA 7、MLA 9、Chicago 作者年份、Vancouver 或 IEEE）。生成服务沿用各自已有的登录、模型与连接器配置。
3. 输入一个观点，点击生成。插件先让所选服务提取英文检索词，再调用 Minicod 的 `POST /v1/stc/papers/search` 查找 Semantic Scholar 论文，最后用有摘要的前四条记录生成带夹注的句子。

完成的结果在 Buffer 中显示为上下两个独立的输出框：上框是带夹注的正文，下框是对应参考文献。每个框右侧有自己的复制按钮；复制后 Buffer 保持打开，可继续复制另一框。发送时两部分仍按“正文 + 换行 + 参考文献”作为一个原子块交付。切换服务、格式、插件或修改原文会取消旧请求并清除旧结果。没有摘要、没有可验证的文献标记，或模型判断没有足够佐证时，插件不提供可发送的结果。

引用使用 Minicod 提供的元数据生成简式格式；论文类型、作者全名、卷期、页码等字段可能缺失。投稿或正式引用前，应打开来源记录核对文献与论断。此版本固定使用 Semantic Scholar 检索，不做跨来源合并，也不调用 Minicod 的 Chat Completions。

本机离线验证：`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/swift build -c debug`，然后运行 `.build/debug/RimeBuffer scholay-plugin-smoke`。该 smoke 使用模拟论文和模拟生成服务，不消耗 Minicod 额度。

## Polisher

选择 Polisher，在工具栏选择 AI 连接器，输入文字后点击生成。结果是保留原意、事实、引文与不确定性的学术语言改写，可以复制或投递。此插件不调用 Minicod 检索，也不添加文献。

## LaTeX

选择 LaTeX，在工具栏选择 AI 连接器与「自然语言 → LaTeX」或「PNG → LaTeX」。前者输入公式描述；后者把公式图片粘贴到 Buffer。图片显示为带缩略图的小卡片，点击 × 或在卡片位于末尾时按退格删除。图片会交给所选 AI 连接器的图像输入接口；结果为可复制或投递的 LaTeX 文本。PNG 模式最多接受三张图片，单张压缩后最多 4 MiB。通用 AI API 的路由还需要选择支持图像内容的模型。

`scholay-academic-smoke` 离线检查出品方、图片的剪贴板解码、非文本投递保护、图像请求、LaTeX 结果与删除后失效。
