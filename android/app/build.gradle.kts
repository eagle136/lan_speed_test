plugins {
    id("com.android.application")
}

// NDK 路径不写死，按优先级查找：
//   1. -PndkPath=<path> 命令行参数
//   2. local.properties 里的 ndkPath=<path>（本机私有，不进仓库）
//   3. 都没有 → AGP 按 ndkVersion 在 sdk.dir/ndk/<version> 下找（CI 走这条）
val localPropsFile = rootProject.file("local.properties")
val localPropsText: String = if (localPropsFile.exists()) localPropsFile.readText() else ""
val localPropsMap: Map<String, String> = localPropsText.lineSequence()
    .map { it.trim() }
    .filter { it.isNotEmpty() && !it.startsWith("#") && it.contains('=') }
    .associate { it.substringBefore('=').trim() to it.substringAfter('=').trim() }
val ndkPathOverride: String? =
    ((project.findProperty("ndkPath") as String?) ?: localPropsMap["ndkPath"])
        ?.takeIf { it.isNotBlank() }

// CI 上若 SDK 仓库还没有 API 37，可用 -PcompileSdk=36 降级编译；本机不传就用 37。
val compileSdkOverride: Int? =
    (project.findProperty("compileSdk") as String?)?.toIntOrNull()

android {
    namespace = "com.eagle.lanspeed"
    compileSdk = compileSdkOverride ?: 37
    buildToolsVersion = "36.0.0"
    ndkVersion = "27.2.12479018"
    if (ndkPathOverride != null) {
        ndkPath = ndkPathOverride
    }

    defaultConfig {
        applicationId = "com.eagle.lanspeed"
        minSdk = 24
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0"
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    externalNativeBuild {
        ndkBuild {
            path = file("src/main/jni/Android.mk")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}
