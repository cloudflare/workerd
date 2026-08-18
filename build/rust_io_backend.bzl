"""Build-graph hermeticity check + shared toggles for workerd's Rust I/O backend.

An aspect walks the transitive dependency graph of a guarded target and, in the rust I/O
config, fails ANALYSIS if the graph reaches a forbidden concrete C++ I/O library (the ones
the rust layer replaces), printing one example dependency path to each offender.

Why a graph assertion and not a link check: if kj-async-os sneaks back into the rust-config
graph, the failure mode is a DOUBLE-DEFINITION (kj::setupAsyncIo and kj::UnixEventPort's
members are defined by both the native TUs and the tokio shim), and with static archives the
winner is link-order-dependent -- possibly a loud duplicate-symbol error, possibly the wrong
event loop silently winning. Undefined-symbol errors only backstop the opposite (absent-lib)
direction. This aspect catches the double-definition direction deterministically, at analysis.

IMPORTANT -- the gate is opt-in: //src/workerd/server:rust-io-hermeticity is tagged `manual`,
so `bazel build //...` never runs it. The rust-config CI lane must build it EXPLICITLY for
the guarantee to hold.

The forbidden set grows in lockstep with what `=rust` means per migration stage (v1: the
tokio event loop + I/O; later stages append kj-http/kj-tls, then capnp-rpc). Deliberately
ALLOWED today: @capnp-cpp//src/kj:kj-async-core and :kj-async-io (the abstract Promise and
stream/Network layers the rust backend itself is built on), kj-http/kj-tls/capnp-rpc (still
the only implementation in both configs; they consume abstract streams, no OS I/O), and the
kj-gzip/kj-brotli codecs (not a transport).
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

# Forbidden concrete C++ I/O targets, as "//package:target" label suffixes (suffix-matched so
# bzlmod repo-name canonicalization doesn't have to be spelled out). The single source of truth.
_FORBIDDEN = [
    # The kj OS event loop / socket layer (setupAsyncIo, UnixEventPort/Win32IocpEventPort),
    # replaced by kj-rs-tokio + kj-rs-io.
    "//src/kj:kj-async-os",
]

RustIoForbiddenInfo = provider(
    doc = "One example dependency path to each forbidden C++ I/O target a subgraph reaches.",
    fields = {
        "paths": "dict of forbidden-label-suffix -> example path (list of label strings)",
    },
)

def _forbidden_suffix(label_str):
    for suffix in _FORBIDDEN:
        if label_str.endswith(suffix):
            return suffix
    return None
