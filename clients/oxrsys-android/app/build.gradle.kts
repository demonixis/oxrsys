// SPDX-License-Identifier: MPL-2.0

plugins {
    id("com.android.application")
}

val oxrsysVersionFile = rootProject.file("../../config/OXRSysVersion.xcconfig")
require(oxrsysVersionFile.isFile) {
    "OXRSys version config is missing at ${oxrsysVersionFile.path}"
}
val oxrsysVersionConfig = oxrsysVersionFile.readLines()
    .map { it.substringBefore("//").trim() }
    .filter { it.contains("=") }
    .associate {
        val parts = it.split("=", limit = 2)
        parts[0].trim() to parts[1].trim()
    }
val oxrsysVersion = oxrsysVersionConfig["OXRSYS_VERSION"]
    ?: error("OXRSYS_VERSION is missing from ${oxrsysVersionFile.path}")
require(Regex("""\d+\.\d+\.\d+""").matches(oxrsysVersion)) {
    "OXRSYS_VERSION must use MAJOR.MINOR.PATCH in ${oxrsysVersionFile.path}"
}
val oxrsysBuild = oxrsysVersionConfig["OXRSYS_BUILD"]?.toIntOrNull()
    ?: error("OXRSYS_BUILD must be an integer in ${oxrsysVersionFile.path}")

val preferredDisplayRefreshRateHz =
    providers.gradleProperty("oxrsysAndroidDisplayRefreshRateHz").orElse("72")

android {
    namespace = "net.demonixis.oxrsys.android"
    compileSdk = 35

    lint {
        abortOnError = false
    }

    defaultConfig {
        applicationId = "net.demonixis.oxrsys.android"
        minSdk = 29          // Quest 2 minimum
        targetSdk = 32       // Meta recommends targetSdk 32 for Quest
        versionCode = oxrsysBuild
        versionName = oxrsysVersion

        ndk {
            abiFilters += "arm64-v8a"  // Quest/Pico are all arm64
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_PLATFORM=android-29",
                    "-DOXRSYS_BUILD=$oxrsysBuild",
                    "-DOXRSYS_PREFERRED_DISPLAY_REFRESH_RATE_HZ=${preferredDisplayRefreshRateHz.get()}"
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
