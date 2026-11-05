import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Release signing: the keystore and its password live outside the repository, in a properties file
// (storeFile, storePassword, keyAlias, keyPassword). Default ~/.config/replaynes/android-signing.properties,
// overridable with REPLAYNES_ANDROID_SIGNING. Without it, assembleRelease produces an unsigned APK.
val signingPropsFile = file(
    System.getenv("REPLAYNES_ANDROID_SIGNING")
        ?: "${System.getProperty("user.home")}/.config/replaynes/android-signing.properties"
)
val signingProps = Properties().apply {
    if (signingPropsFile.isFile) signingPropsFile.inputStream().use { load(it) }
}

android {
    namespace = "io.github.replaynes"
    compileSdk = 35
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "io.github.replaynes"
        minSdk = 26
        targetSdk = 35
        // versionName follows project(ReplayNES VERSION ...) in the top-level CMakeLists.txt.
        versionCode = 7
        versionName = "0.6.5"
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                // The Nestopia core is far too slow unoptimized: always build the native code as
                // Release (also for the debug variant). See src/main/cpp/CMakeLists.txt.
                arguments += listOf("-DANDROID_STL=c++_static", "-DCMAKE_BUILD_TYPE=Release")
            }
        }
    }

    signingConfigs {
        if (signingProps.getProperty("storeFile") != null) {
            create("release") {
                storeFile = file(signingProps.getProperty("storeFile"))
                storePassword = signingProps.getProperty("storePassword")
                keyAlias = signingProps.getProperty("keyAlias")
                keyPassword = signingProps.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        debug {
            applicationIdSuffix = ".dev"
            versionNameSuffix = "-dev"
        }
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}
