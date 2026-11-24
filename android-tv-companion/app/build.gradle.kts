plugins {
    id("com.android.application")
}

android {
    namespace = "de.esp32adb.companion"
    compileSdk = 36

    defaultConfig {
        applicationId = "de.esp32adb.companion"
        minSdk = 21
        targetSdk = 36
        versionCode = 8
        versionName = "0.8.0"
    }
}
