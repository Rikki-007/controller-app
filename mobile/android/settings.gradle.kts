pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "vcd-controller"

// The shared C core and its Kotlin API. The app module (UI) goes next to it
// and depends on it: implementation(project(":vccore")).
include(":vccore")
