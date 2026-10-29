import java.util.Properties
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val local = Properties().apply {
    val f = rootProject.file("local.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
val amapKey = (local.getProperty("AMAP_KEY") ?: System.getenv("AMAP_KEY") ?: "").trim()

android {
    namespace = "com.example.navbridge"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.example.navbridge"
        minSdk = 26
        targetSdk = 35
        // 每次重新出包必须递增，便于区分设备上旧包/新包。
        versionCode = 18
        versionName = "0.8.6"

        buildConfigField("String", "AMAP_KEY", "\"$amapKey\"")
        manifestPlaceholders["AMAP_KEY"] = amapKey

        // 高德 10.x 只带常用 ABI；现代手机基本是 arm64
        ndk { abiFilters += setOf("arm64-v8a") }
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs { useLegacyPackaging = true }
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }

    lint {
        abortOnError = false
        checkReleaseBuilds = false
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    // 高德：导航 + 3D 地图 + 定位 + 搜索，合并包，避免版本冲突
    implementation("com.amap.api:navi-3dmap-location-search:10.1.600_3dmap10.1.600_loc6.5.1_sea9.7.4")
}
