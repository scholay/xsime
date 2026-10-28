# 键盘布局编辑器原型

本地 HTML/CSS/JavaScript 原型，用于演示键位交换、行布局、尺寸、分区和试打预览。支持浏览器内保存及配置导入导出；试打只展示键位行为，不运行 Rime 或生成中文候选。

在仓库根目录启动：

```sh
python3 -m http.server 8768 --bind 127.0.0.1 --directory platforms/ios/Design/keyboard-layout-editor
```

打开 `http://127.0.0.1:8768/`。布局保存在当前浏览器和访问地址下，换端口或浏览器前可先导出。生产键盘的布局设置入口是 [`CustomKeyboardLayoutsView.swift`](../../App/CustomKeyboardLayoutsView.swift)。
