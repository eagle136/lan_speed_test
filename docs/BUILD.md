# 从源码构建

两端各自独立构建，互不依赖。CI 也会做同样的事（见 `.github/workflows/release.yml`）。

---

## 1. Windows PC 端

### 依赖

| 项 | 要求 |
|---|---|
| 编译器 | MinGW-w64 GCC（MSYS2 `mingw64` 环境，或 WinLibs 独立包）。已验证 GCC 16.2.0 |
| 系统库 | `ws2_32`、`iphlpapi`（`GetIpAddrTable` 用于取本机局域网地址） |
| 运行时 | 无。静态链接，产物是单文件 exe |

不建议用 MSVC：`pc_main.c` 用了 `-municode` 的 `wWinMain` 入口和部分 GNU 扩展写法，
换 MSVC 需要改入口与源码字符集处理，没必要。

### 构建

```bat
pc\build.bat
```

等价的手工命令（在仓库根目录执行）：

```bat
gcc -O2 -std=c11 -municode -mwindows -static -s -Icore ^
    pc\pc_main.c core\ls_core.c core\ls_disc.c ^
    -o dist\LanSpeedTest-PC.exe -lws2_32 -liphlpapi
```

关键参数为什么这么写：

| 参数 | 原因 |
|---|---|
| `-municode` | 源码入口是 `wWinMain`（宽字符版），缺了会报 `undefined reference to WinMain` |
| `-mwindows` | GUI 子系统，不弹控制台黑框 |
| `-static` | 不依赖 `libgcc_s_seh-1.dll` / `libwinpthread-1.dll`，拷到别的机器直接能跑 |
| `-s` | 去掉符号表，体积从 ~400 KB 降到 ~150 KB |
| `-liphlpapi` | `ls_disc.c` 用 `GetIpAddrTable` 取本机地址，漏了会链接失败 |
| `-Icore` | `#include "ls_core.h"` 的相对路径靠它解析 |

产物：`dist/LanSpeedTest-PC.exe`。

## 2. Android 端

### 依赖

| 项 | 要求 |
|---|---|
| JDK | 17 或更高 |
| Android SDK | `compileSdk 37`、`build-tools 36.0.0` |
| NDK | `27.2.12479018`（r27c）。必须带，JNI 层要编 `arm64-v8a` |
| Gradle | 工程自带 wrapper（`gradlew.bat`），无需本机安装 |

### 配置

`android/local.properties` 保存本机 SDK 路径，**已被 .gitignore 排除**。新机器上：

```properties
# android/local.properties
sdk.dir=C\:\\Users\\<你>\\AppData\\Local\\Android\\Sdk
```

NDK 路径有三种给法，任选一种：

```properties
# 方案 A：写进 android/local.properties（本机私有，已被 .gitignore 排除）
ndkPath=C:/android-ndk/android-ndk-r27c
```

```bat
:: 方案 B：命令行临时指定
gradlew.bat assembleRelease -PndkPath=C:/android-ndk/android-ndk-r27c
```

两者都不给时，AGP 会按 `ndkVersion` 在 `sdk.dir\ndk\27.2.12479018` 下找。
`build.gradle.kts` 里不写死绝对路径，是为了让 CI 能编过——这是硬要求，改回去会直接让 release 流水线挂掉。

### 构建

```bat
cd android
gradlew.bat assembleRelease
```

首次构建会下载 Gradle 9.5 与 AGP，约几分钟。产物：

```
android/app/build/outputs/apk/release/app-release.apk
```

`release` 构建类型当前复用 **debug 签名**（`signingConfig = signingConfigs.getByName("debug")`），
所以能直接安装，但升级时要求前后版本签名一致。要正式签名就自己加 `keystore.properties`
（记得加进 .gitignore）。

### ABIs

`abiFilters` 只有 `arm64-v8a`，`APP_SUPPORT_FLEXIBLE_PAGE_SIZES := true`（16 KB page size 对齐，
Android 15+ 的要求，`Android.mk` 里的 `-Wl,-z,max-page-size=16384` 与之配套）。
加 `armeabi-v7a` 需要同时改 `Application.mk` 的 `APP_ABI` 和 `abiFilters`。

## 3. 核心回环自检

不连任何网络，在一台机器上开两个会话走完整流程（端口 45201，每方向 2 秒）：

```bat
tests\build_and_run.bat
```

或者手工：

```bat
gcc -O2 -std=c11 -Icore tests\ls_selftest.c core\ls_core.c core\ls_disc.c ^
    -o build\ls_selftest.exe -lws2_32 -liphlpapi
build\ls_selftest.exe 1     :: 1 = 启用流水线线程，0 = 关闭
```

通过标志是最后一行 `SELFTEST OK`（退出码 0）。判定条件：

- 两端 `phase == DONE`，四个方向都有数据
- 两端 `up_mbps` / `down_mbps` 差值 < 0.01 Mbps（统计口径一致性的硬断言）

改 `core/` 里任何统计或时序代码后先跑它，再上真机。

## 4. 版本号与产物命名

- 版本号写在 `android/app/build.gradle.kts`（`versionCode` / `versionName`）与 git tag 两处，
  必须同步；PC 端不嵌版本号，以 tag 为准。
- Release 资产命名固定：

```
LanSpeedTest-PC-v<version>.exe
LanSpeedTest-Android-arm64-v<version>.apk
```

- 发布流程：改版本号 → 更新 `CHANGELOG.md` → commit → `git tag v<version>` → `git push --tags`。
  workflow 会自动构建两端、生成 Release 并附带 md5。

## 5. 常见构建错误

| 报错 | 原因 |
|---|---|
| `undefined reference to WinMain` | 漏了 `-municode` |
| `undefined reference to GetIpAddrTable` | 漏了 `-liphlpapi` |
| `ls_core.h: No such file or directory` | 漏了 `-Icore` |
| `NDK not configured` | 三个路径方案都没提供，或 `ndkVersion` 与实际安装版本不符 |
| `SDK location not found` | 缺 `android/local.properties` |
| `Plugin [id: 'com.android.application'] was not found` | 没联网拉 AGP，或 Gradle 版本过低（需 9.x） |
