"""Repository rule for building doxygen from source with libclang support.

This rule downloads and builds doxygen from source using CMake, linking against
the system LLVM/clang installation to enable clang-assisted parsing of C++ code.
"""

def _doxygen_repository_impl(ctx):
    """Builds doxygen from source with libclang support."""

    # Download doxygen source
    ctx.download_and_extract(
        url = ctx.attr.urls,
        sha256 = ctx.attr.sha256,
        stripPrefix = ctx.attr.strip_prefix,
    )

    # Create the build script
    build_script = """#!/bin/bash
set -euo pipefail

cd "{src_dir}"
mkdir -p build
cd build

# Find LLVM installation
LLVM_DIR=""
for v in 21 20 19 18; do
    if [ -d "/usr/lib/llvm-$v" ]; then
        LLVM_DIR="/usr/lib/llvm-$v"
        break
    fi
done

if [ -z "$LLVM_DIR" ]; then
    echo "ERROR: No LLVM installation found" >&2
    exit 1
fi

echo "Using LLVM from: $LLVM_DIR"

# Configure with CMake
cmake .. \\
    -DCMAKE_BUILD_TYPE=Release \\
    -DCMAKE_C_COMPILER=clang \\
    -DCMAKE_CXX_COMPILER=clang++ \\
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \\
    -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld" \\
    -Duse_libclang=ON \\
    -Duse_libc++=OFF \\
    -DLLVM_DIR="$LLVM_DIR/lib/cmake/llvm" \\
    -DClang_DIR="$LLVM_DIR/lib/cmake/clang" \\
    -DCMAKE_INSTALL_PREFIX="{install_dir}"

# Build
make -j$(nproc)
make install
""".format(
        src_dir = ctx.path("."),
        install_dir = ctx.path("install"),
    )

    ctx.file("build_doxygen.sh", build_script, executable = True)

    # Execute the build
    result = ctx.execute(
        ["bash", "build_doxygen.sh"],
        timeout = 1800,  # 30 minutes
        quiet = False,
    )

    if result.return_code != 0:
        fail("Failed to build doxygen:\nstdout:\n{}\nstderr:\n{}".format(
            result.stdout,
            result.stderr,
        ))

    # Create BUILD file exposing the doxygen binary
    ctx.file("BUILD.bazel", """
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "doxygen_bin",
    srcs = ["install/bin/doxygen"],
)

filegroup(
    name = "doxygen",
    srcs = ["install/bin/doxygen"],
)
""")

doxygen_repository = repository_rule(
    implementation = _doxygen_repository_impl,
    attrs = {
        "urls": attr.string_list(
            mandatory = True,
            doc = "URLs to download doxygen source archive",
        ),
        "sha256": attr.string(
            mandatory = True,
            doc = "SHA256 checksum of the archive",
        ),
        "strip_prefix": attr.string(
            mandatory = True,
            doc = "Prefix to strip from the archive",
        ),
    },
    doc = "Downloads and builds doxygen from source with libclang support",
)
