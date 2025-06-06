"""Repository rule for extracting versions from dependency bzl files."""

def _extract_versions_impl(ctx):
    """Implementation of extract_versions repository rule."""

    # Read the ada-url bzl file
    ada_bzl_content = ctx.read(ctx.path(ctx.workspace_root.get_child("build").get_child("deps").get_child("gen").get_child("dep_ada_url.bzl")))

    # Extract TAG_NAME using Python-like string operations
    tag_start = ada_bzl_content.find('TAG_NAME = "')
    if tag_start == -1:
        fail("Could not find TAG_NAME in dep_ada_url.bzl")

    tag_start += len('TAG_NAME = "')
    tag_end = ada_bzl_content.find('"', tag_start)
    if tag_end == -1:
        fail("Could not parse TAG_NAME in dep_ada_url.bzl")

    tag_name = ada_bzl_content[tag_start:tag_end]

    # Remove 'v' prefix if present
    ada_version = tag_name[1:] if tag_name.startswith("v") else tag_name

    # Read brotli version
    brotli_bzl_content = ctx.read(ctx.path(ctx.workspace_root.get_child("build").get_child("deps").get_child("gen").get_child("dep_brotli.bzl")))

    brotli_start = brotli_bzl_content.find('TAG_NAME = "')
    if brotli_start == -1:
        fail("Could not find TAG_NAME in dep_brotli.bzl")

    brotli_start += len('TAG_NAME = "')
    brotli_end = brotli_bzl_content.find('"', brotli_start)
    if brotli_end == -1:
        fail("Could not parse TAG_NAME in dep_brotli.bzl")

    brotli_tag = brotli_bzl_content[brotli_start:brotli_end]
    brotli_version = brotli_tag[1:] if brotli_tag.startswith("v") else brotli_tag

    # Read simdutf version
    simdutf_bzl_content = ctx.read(ctx.path(ctx.workspace_root.get_child("build").get_child("deps").get_child("gen").get_child("dep_simdutf.bzl")))

    simdutf_start = simdutf_bzl_content.find('TAG_NAME = "')
    if simdutf_start == -1:
        fail("Could not find TAG_NAME in dep_simdutf.bzl")

    simdutf_start += len('TAG_NAME = "')
    simdutf_end = simdutf_bzl_content.find('"', simdutf_start)
    if simdutf_end == -1:
        fail("Could not parse TAG_NAME in dep_simdutf.bzl")

    simdutf_tag = simdutf_bzl_content[simdutf_start:simdutf_end]
    simdutf_version = simdutf_tag[1:] if simdutf_tag.startswith("v") else simdutf_tag

    # Get Node.js version from attribute
    node_version = ctx.attr.node_version

    # Parse SQLite version from WORKSPACE
    workspace_content = ctx.read(ctx.path(ctx.workspace_root.get_child("WORKSPACE")))

    sqlite_start = workspace_content.find('strip_prefix = "sqlite-src-')
    if sqlite_start == -1:
        fail("Could not find sqlite strip_prefix in WORKSPACE")

    sqlite_start += len('strip_prefix = "sqlite-src-')
    sqlite_end = workspace_content.find('"', sqlite_start)
    if sqlite_end == -1:
        fail("Could not parse sqlite version in WORKSPACE")

    sqlite_raw = workspace_content[sqlite_start:sqlite_end]

    # Convert format like "3470000" to "3.47.0"
    if len(sqlite_raw) == 7:
        sqlite_version = sqlite_raw[0] + "." + sqlite_raw[1:3] + "." + sqlite_raw[3:5]
    else:
        sqlite_version = sqlite_raw

    # Parse nbytes version from WORKSPACE
    nbytes_start = workspace_content.find('strip_prefix = "nbytes-')
    if nbytes_start == -1:
        fail("Could not find nbytes strip_prefix in WORKSPACE")

    nbytes_start += len('strip_prefix = "nbytes-')
    nbytes_end = workspace_content.find('"', nbytes_start)
    if nbytes_end == -1:
        fail("Could not parse nbytes version in WORKSPACE")

    nbytes_version = workspace_content[nbytes_start:nbytes_end]

    # Parse ncrypto version from WORKSPACE
    ncrypto_start = workspace_content.find('strip_prefix = "ncrypto-')
    if ncrypto_start == -1:
        fail("Could not find ncrypto strip_prefix in WORKSPACE")

    ncrypto_start += len('strip_prefix = "ncrypto-')
    ncrypto_end = workspace_content.find('"', ncrypto_start)
    if ncrypto_end == -1:
        fail("Could not parse ncrypto version in WORKSPACE")

    ncrypto_version = workspace_content[ncrypto_start:ncrypto_end]

    # Read V8 version
    v8_bzl_content = ctx.read(ctx.path(ctx.workspace_root.get_child("build").get_child("deps").get_child("v8.bzl")))

    v8_start = v8_bzl_content.find('VERSION = "')
    if v8_start == -1:
        fail("Could not find VERSION in v8.bzl")

    v8_start += len('VERSION = "')
    v8_end = v8_bzl_content.find('"', v8_start)
    if v8_end == -1:
        fail("Could not parse VERSION in v8.bzl")

    v8_version = v8_bzl_content[v8_start:v8_end]

    # Generate the versions.h header file
    header_content = """// This file is auto-generated. Do not edit manually.
#pragma once

#include <kj/string.h>

namespace workerd::api::node {{

// Version of the ada-url library
static constexpr kj::StringPtr adaVersion = "{ada_version}"_kj;

// Version of Node.js
static constexpr kj::StringPtr nodeVersion = "{node_version}"_kj;

// Version of Brotli compression library
static constexpr kj::StringPtr brotliVersion = "{brotli_version}"_kj;

// Version of simdutf library
static constexpr kj::StringPtr simdutfVersion = "{simdutf_version}"_kj;

// Version of SQLite database
static constexpr kj::StringPtr sqliteVersion = "{sqlite_version}"_kj;

// Version of nbytes library
static constexpr kj::StringPtr nbytesVersion = "{nbytes_version}"_kj;

// Version of ncrypto library
static constexpr kj::StringPtr ncryptoVersion = "{ncrypto_version}"_kj;

// Version of V8 JavaScript engine
static constexpr kj::StringPtr v8Version = "{v8_version}"_kj;

}}  // namespace workerd::api::node
""".format(ada_version = ada_version, node_version = node_version, brotli_version = brotli_version, simdutf_version = simdutf_version, sqlite_version = sqlite_version, nbytes_version = nbytes_version, ncrypto_version = ncrypto_version, v8_version = v8_version)

    # Write the header file
    ctx.file("versions.h", header_content)

    # Create a BUILD file to export the header
    build_content = """
exports_files(["versions.h"], visibility = ["//visibility:public"])

cc_library(
    name = "versions",
    hdrs = ["versions.h"],
    visibility = ["//visibility:public"],
)
"""

    ctx.file("BUILD.bazel", build_content)

_extract_versions = repository_rule(
    implementation = _extract_versions_impl,
    doc = "Extracts versions from dependency bzl files and generates C++ headers",
    attrs = {
        "node_version": attr.string(
            mandatory = True,
            doc = "The Node.js version string",
        ),
    },
)

def extract_versions(node_version):
    """Generate version headers from dependency bzl files."""
    _extract_versions(
        name = "generated_versions",
        node_version = node_version,
    )
