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

def _rust_io_forbidden_aspect_impl(target, ctx):
    label_str = str(target.label)
    paths = {}

    hit = _forbidden_suffix(label_str)
    if hit != None:
        paths[hit] = [label_str]

    # Scan ctx.rule.attr generically (attr_aspects = ["*"]) rather than enumerating
    # deps/implementation_deps/etc. per rule kind.
    for attr_name in dir(ctx.rule.attr):
        value = getattr(ctx.rule.attr, attr_name)
        dep_targets = []
        if type(value) == "list":
            for item in value:
                if type(item) == "Target":
                    dep_targets.append(item)
        elif type(value) == "Target":
            dep_targets.append(value)

        for dep in dep_targets:
            if RustIoForbiddenInfo in dep:
                for forbidden, subpath in dep[RustIoForbiddenInfo].paths.items():
                    if forbidden not in paths:
                        paths[forbidden] = [label_str] + subpath

    return [RustIoForbiddenInfo(paths = paths)]

rust_io_forbidden_aspect = aspect(
    implementation = _rust_io_forbidden_aspect_impl,
    attr_aspects = ["*"],
    doc = "Propagates RustIoForbiddenInfo up the dependency graph.",
)

def _rust_io_hermeticity_impl(ctx):
    info = ctx.attr.target[RustIoForbiddenInfo]
    paths = info.paths

    report = ctx.actions.declare_file(ctx.label.name + ".txt")

    if ctx.attr.enforce and len(paths) > 0:
        lines = [
            "",
            "Rust-I/O hermeticity FAILED for {} in the rust I/O config.".format(
                str(ctx.attr.target.label),
            ),
            "",
            "Its transitive dependency graph reaches {} forbidden concrete C++ ".format(len(paths)) +
            "I/O target(s) that the rust I/O layer (the tokio event loop + tokio",
            "sockets) is meant to keep off the build. Each is a real migration item; the",
            "example dependency edge shows one path that pulls it in:",
            "",
        ]
        for forbidden in sorted(paths.keys()):
            path = paths[forbidden]
            lines.append("  [FORBIDDEN] {}".format(forbidden))
            lines.append("    reached via:")
            for i, hop in enumerate(path):
                lines.append("      {}{}".format("  " * i, hop))
            lines.append("")
        lines.append(
            "Fix by removing the offending edge (migrate the code to the tokio-backed I/O layer,",
        )
        lines.append(
            "or drop the forbidden dep from that target's deps under select(io_backend_rust)),",
        )
        lines.append(
            "or -- if this is a deliberate remaining kj-mode/in-process site -- adjust the",
        )
        lines.append(
            "forbidden set in build/rust_io_backend.bzl (a reviewable change).",
        )
        fail("\n".join(lines))

    # Not enforcing (cxx config), or clean: emit a report artifact and a summary.
    if len(paths) == 0:
        summary = "rust-io-hermeticity: OK -- {} reaches 0 forbidden C++ I/O targets.".format(
            str(ctx.attr.target.label),
        )
    else:
        summary = ("rust-io-hermeticity: {} reaches {} forbidden C++ I/O target(s) " +
                   "(NOT enforced in this config; --//:io_backend=rust would fail): {}").format(
            str(ctx.attr.target.label),
            len(paths),
            ", ".join(sorted(paths.keys())),
        )
    ctx.actions.write(report, summary + "\n")
    return [DefaultInfo(files = depset([report]))]
