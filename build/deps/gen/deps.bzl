# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//build/deps:gen/dep_capnp_cpp.bzl", "dep_capnp_cpp")
load("@//build/deps:gen/dep_com_google_protobuf.bzl", "dep_com_google_protobuf")
load("@//build/deps:gen/dep_dawn.bzl", "dep_dawn")
load("@//build/deps:gen/dep_spirv_headers.bzl", "dep_spirv_headers")
load("@//build/deps:gen/dep_ssl.bzl", "dep_ssl")
load("@//build/deps:gen/dep_vulkan_headers.bzl", "dep_vulkan_headers")
load("@//build/deps:gen/dep_vulkan_utility_libraries.bzl", "dep_vulkan_utility_libraries")

def deps_gen():
    dep_capnp_cpp()
    dep_ssl()
    dep_dawn()
    dep_vulkan_utility_libraries()
    dep_vulkan_headers()
    dep_spirv_headers()
    dep_com_google_protobuf()
