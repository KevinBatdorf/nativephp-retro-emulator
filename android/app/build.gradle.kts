plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.kevinbatdorf.plugins.retroemulator"

    sourceSets {
        getByName("main") {
            // Runtime classes live in resources/android/ — the canonical copy
            // that AndroidPluginCompiler ships into host apps. Compiling them
            // here keeps the plugin's own test app and hosts on one source.
            // EmulatorFunctions.kt depends on host-app NativePHP types and
            // only compiles inside a host build.
            kotlin.srcDir("../../resources/android")
        }
    }
    compileSdk = 35
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.kevinbatdorf.plugins.retroemulator"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // Phase 4: arm64-v8a for device, x86_64 for emulator testing
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++20"
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DCMAKE_BUILD_TYPE=Release"
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    kotlinOptions {
        jvmTarget = "11"
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    // ActivityScenario for Phase 4 rendering test
    androidTestImplementation("androidx.test:core:1.6.1")
}

// These depend on host-app NativePHP types (and Compose) and only compile
// inside a host build — exclude them from the plugin's own test app.
// DpadResolver.kt stays in, so its behaviour is testable on device.
tasks.withType<org.jetbrains.kotlin.gradle.tasks.KotlinCompile>().configureEach {
    exclude("**/EmulatorFunctions.kt")
    exclude("**/EmulatorSurface.kt")
    exclude("**/DpadSurface.kt")
}
