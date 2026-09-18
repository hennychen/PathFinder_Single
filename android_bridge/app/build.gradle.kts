plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.pathfinder.navbridge"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.pathfinder.navbridge"
        minSdk = 24
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    // 高德导航 SDK 合包 (含地图/导航/定位)。注意: Maven Central 上的真实版本号带 _3dmap 后缀
    implementation("com.amap.api:navi-3dmap:10.0.800_3dmap10.0.800")
    implementation("androidx.appcompat:appcompat:1.7.0")
}
