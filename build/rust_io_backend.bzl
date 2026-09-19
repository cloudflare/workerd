"""Shared toggle for workerd's Rust I/O backend (--//:io_backend).

The dependency side of the backend switch lives in //src/rust/cxx/kj-rs-io:active-backend (one
select over the per-backend deps); this file carries the compile-time side: the define read by
the translation units that have a per-backend arm.

Hermeticity of the rust config is checked in two places. kj-async-os (kj's own event loop and
sockets) must be ABSENT from the workerd link, or its kj::setupAsyncIo() / kj::UnixEventPort
definitions collide with the tokio shim's and the linker silently keeps whichever archive it meets
first; likewise kj-http-impl (kj's HTTP/1.1 implementation, whose kj:: symbols
//src/workerd/util:kj-http defines over hyper) and kj-tls (replaced by rustls).
  * build/rust_io_graph_check.sh -- `bazel cquery somepath(...)` over the dependency graph
    (`just check-io-backend-graph`; the lint CI lane runs it);
  * //src/workerd/server:rust-io-link-check -- the linked binary's symbol names.
"""

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
