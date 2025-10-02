"""No-op toolchains to avoid rules_android trying to load an NDK"""

load(":noop.bzl", "noop_toolchain")

noop_toolchain(
    name = "noop",
)

toolchain(
    name = "android_sdk_toolchain_impl",
    toolchain_type = "@rules_android//toolchains/android_sdk:toolchain_type",
    toolchain = ":noop",
    visibility = ["//visibility:public"],
)

toolchain(
    name = "android_toolchain_impl",
    toolchain_type = "@rules_android//toolchains/android:toolchain_type",
    toolchain = ":noop",
    visibility = ["//visibility:public"],
)
