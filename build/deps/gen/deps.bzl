# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//build/deps:gen/dep_ada_url.bzl", "dep_ada_url")
load("@//build/deps:gen/dep_brotli.bzl", "dep_brotli")
load("@//build/deps:gen/dep_capnp_cpp.bzl", "dep_capnp_cpp")
load("@//build/deps:gen/dep_com_google_protobuf.bzl", "dep_com_google_protobuf")
load("@//build/deps:gen/dep_simdutf.bzl", "dep_simdutf")
load("@//build/deps:gen/dep_ssl.bzl", "dep_ssl")

def deps_gen():
    dep_capnp_cpp()
    dep_ssl()
    dep_ada_url()
    dep_simdutf()
    dep_com_google_protobuf()
    dep_brotli()
