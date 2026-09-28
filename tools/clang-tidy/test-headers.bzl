"""Expose real dependency headers to standalone clang-tidy fixture tests."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _cc_test_headers_impl(ctx):
    return [DefaultInfo(files = ctx.attr.dep[CcInfo].compilation_context.headers)]

cc_test_headers = rule(
    implementation = _cc_test_headers_impl,
    attrs = {"dep": attr.label(mandatory = True, providers = [CcInfo])},
)
