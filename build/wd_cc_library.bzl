"""wd_cc_library definition"""

load("@rules_cc//cc:cc_library.bzl", "cc_library")

# CPU features enabled for all of workerd's C++, matching the ones enabled for Rust
# (`extra_rustc_flags_triples` in build/deps/rust.MODULE.bazel). Code that LTO may inline across
# targets must agree on them: LLVM doesn't inline a function into one compiled without all of its
# features.
# TODO(cleanup): Come up with a better approach to specify these options for all C/C++ targets - it
# appears to be impossible to set arch-specific copts without a custom toolchain or manually
# setting a config from the CLI interface (which we can do in CI, but can't easily do for local
# builds).
WD_CC_ARCH_COPTS = select({
    "@platforms//cpu:aarch64": [
        "-mcrc",
    ],
    "@platforms//cpu:x86_64": [
        # Note that msse4.2 already includes CRC32C and SSSE3 extensions
        "-msse4.2",
    ],
})

def wd_cc_library(strip_include_prefix = "/src", copts = [], **kwargs):
    """Wrapper for cc_library that sets common attributes
    """
    cc_library(
        copts = copts + WD_CC_ARCH_COPTS,
        strip_include_prefix = strip_include_prefix,
        **kwargs
    )
