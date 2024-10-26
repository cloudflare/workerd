# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//build/deps:gen/dep_aspect_bazel_lib.bzl", "dep_aspect_bazel_lib")
load("@//build/deps:gen/dep_aspect_rules_esbuild.bzl", "dep_aspect_rules_esbuild")
load("@//build/deps:gen/dep_aspect_rules_js.bzl", "dep_aspect_rules_js")
load("@//build/deps:gen/dep_aspect_rules_ts.bzl", "dep_aspect_rules_ts")
load("@//build/deps:gen/dep_bazel_skylib.bzl", "dep_bazel_skylib")
load("@//build/deps:gen/dep_build_bazel_apple_support.bzl", "dep_build_bazel_apple_support")
load("@//build/deps:gen/dep_cargo_bazel_linux_arm64.bzl", "dep_cargo_bazel_linux_arm64")
load("@//build/deps:gen/dep_cargo_bazel_linux_x64.bzl", "dep_cargo_bazel_linux_x64")
load("@//build/deps:gen/dep_cargo_bazel_macos_arm64.bzl", "dep_cargo_bazel_macos_arm64")
load("@//build/deps:gen/dep_cargo_bazel_macos_x64.bzl", "dep_cargo_bazel_macos_x64")
load("@//build/deps:gen/dep_cargo_bazel_win_x64.bzl", "dep_cargo_bazel_win_x64")
load("@//build/deps:gen/dep_clang_format_darwin_arm64.bzl", "dep_clang_format_darwin_arm64")
load("@//build/deps:gen/dep_clang_format_linux_amd64.bzl", "dep_clang_format_linux_amd64")
load("@//build/deps:gen/dep_clang_format_linux_arm64.bzl", "dep_clang_format_linux_arm64")
load("@//build/deps:gen/dep_com_google_benchmark.bzl", "dep_com_google_benchmark")
load("@//build/deps:gen/dep_cxxbridge_cmd.bzl", "dep_cxxbridge_cmd")
load("@//build/deps:gen/dep_platforms.bzl", "dep_platforms")
load("@//build/deps:gen/dep_rules_cc.bzl", "dep_rules_cc")
load("@//build/deps:gen/dep_rules_java.bzl", "dep_rules_java")
load("@//build/deps:gen/dep_rules_nodejs.bzl", "dep_rules_nodejs")
load("@//build/deps:gen/dep_rules_python.bzl", "dep_rules_python")
load("@//build/deps:gen/dep_rules_rust.bzl", "dep_rules_rust")
load("@//build/deps:gen/dep_rules_shell.bzl", "dep_rules_shell")

def deps_gen():
    dep_bazel_skylib()
    dep_platforms()
    dep_rules_python()
    dep_build_bazel_apple_support()
    dep_rules_rust()
    dep_cargo_bazel_linux_x64()
    dep_cargo_bazel_linux_arm64()
    dep_cargo_bazel_macos_x64()
    dep_cargo_bazel_macos_arm64()
    dep_cargo_bazel_win_x64()
    dep_cxxbridge_cmd()
    dep_rules_cc()
    dep_rules_java()
    dep_rules_shell()
    dep_aspect_rules_esbuild()
    dep_rules_nodejs()
    dep_aspect_bazel_lib()
    dep_aspect_rules_js()
    dep_aspect_rules_ts()
    dep_clang_format_linux_amd64()
    dep_clang_format_linux_arm64()
    dep_clang_format_darwin_arm64()
    dep_com_google_benchmark()
