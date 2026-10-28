# Android 构建与发布

Android 版本来自 [`platforms/android/VERSION`](../../../platforms/android/VERSION)，构建号由 [`app/build.gradle.kts`](../../../platforms/android/app/build.gradle.kts) 的 `versionCode` 提供。正式 applicationId 是 `org.scholay.rimes.android`，开发包带 `.debug` 后缀；升级保留已有签名身份和用户数据。

## 远端构建与签名

[`android.yml`](../../../.github/workflows/android.yml) 执行原生引擎、核心测试、Lint、开发与未签名 Release 构建，以及模拟器触控、导入、更新和包对齐检查。它支持 PR、main 推送、手动运行和其他工作流调用。

正式发布工作流通过 `signed_release` 调用签名任务：仅接受 `v*` 标签，校验标签与根版本一致、源码等于远端 main，再对同次检查生成的 APK/AAB 签名。凭据由 `android-release` 环境提供：

- `RIMES_ANDROID_KEYSTORE_BASE64`
- `RIMES_ANDROID_STORE_PASSWORD`
- `RIMES_ANDROID_KEY_ALIAS`
- `RIMES_ANDROID_KEY_PASSWORD`

签名实现为 [`sign-android-ci.py`](../../../scripts/release/sign-android-ci.py)，产物作为 `RIMES-Android-release-<commit>` 保留。公开资产发布由[全平台发布流程](../../release/README.md)负责。

## 本地签名入口

产品构建与自动测试以远端 CI 为准。维护者需要本地签名包时，先按 [Android README](../../../platforms/android/README.md) 准备 JDK、SDK、固定原生依赖及生成资源，再设置 `ANDROID_HOME` 和以下签名环境变量：

- `RIMES_ANDROID_KEYSTORE`：既有正式 keystore 的绝对路径。
- `RIMES_ANDROID_STORE_PASSWORD`、`RIMES_ANDROID_KEY_ALIAS`、`RIMES_ANDROID_KEY_PASSWORD`。

从仓库根目录运行：

```sh
bash platforms/android/scripts/build-release.sh
```

脚本执行核心测试、Release lint、APK/AAB 构建、签名和 16 KB 对齐验证，并将产物及 `SHA256SUMS` 写入 `platforms/android/dist/<version>/`。缺少签名材料或目标目录已存在时停止，不安装或上传。普通 Gradle Release 构建没有完整签名配置时只产生未签名包。

开发包与正式包身份不同，不能作为彼此的原地升级包。普通升级不卸载应用或清除数据。CI 结果、实际设备验收和公开资产读回分别核对。
