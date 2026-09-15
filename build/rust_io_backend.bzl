"""Shared toggles for workerd's Rust I/O backend (--//:io_backend, see //BUILD.bazel)."""

visibility("public")

def rust_io_backend_local_defines():
    """local_defines for TUs that `#if WORKERD_RUST_IO_BACKEND_RUST` (the declared seam points).

    Kept per-target rather than a repo-global define so the seam stays enumerable: the only
    places allowed to diverge per backend are the targets that ask for this.
    """
    return select({
        "//:io_backend_rust": ["WORKERD_RUST_IO_BACKEND_RUST=1"],
        "//conditions:default": [],
    })
