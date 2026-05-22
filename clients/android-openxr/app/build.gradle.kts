// SPDX-License-Identifier: MPL-2.0

plugins {
    id("com.android.application")
}

val preferredDisplayRefreshRateHz =
    providers.gradleProperty("openxrClientDisplayRefreshRateHz").orElse("72")

android {
    namespace = "com.openxrosx.client"
    compileSdk = 35

    lint {
        abortOnError = false
    }

    defaultConfig {
        applicationId = "com.openxrosx.client"
        minSdk = 29          // Quest 2 minimum
        targetSdk = 32       // Meta recommends targetSdk 32 for Quest
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += "arm64-v8a"  // Quest/Pico are all arm64
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_PLATFORM=android-29",
                    "-DOPENXR_OSX_PREFERRED_DISPLAY_REFRESH_RATE_HZ=${preferredDisplayRefreshRateHz.get()}"
                )
            }
        }
    }

    buildTypes {
        debug {
            isDebuggable = true
        }
        release {
            isMinifyEnabled = false
            isDebuggable = false
            signingConfig = signingConfigs.getByName("debug") // Use debug key for sideloading
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1+"
        }
    }

    // Quest apps are landscape-only, no need for portrait resources
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

dependencies {
    // OpenXR loader is built from source via CMake FetchContent.
    // The critical manifest entries (permissions + queries) that the AAR normally
    // provides via manifest merger are added manually in AndroidManifest.xml.
}
