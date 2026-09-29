#![allow(clippy::missing_errors_doc)]
#![allow(clippy::must_use_candidate)]
#![allow(clippy::unnecessary_box_returns)] // cxx bridge functions return Box<T> by contract

mod test_helpers;

use test_helpers::address_from_loopback_ports;
use test_helpers::create_prebound_listener_fd;

#[cxx::bridge(namespace = "kj_rs_io_test")]
mod ffi {
    /// A pre-bound, *blocking* std TCP listener handed to C++ as a raw fd (the `--socket-fd`
    /// scenario for `wrapListenSocketFd`).
    struct PreboundListener {
        fd: i32,
        port: u16,
    }

    extern "Rust" {
        /// Binds 127.0.0.1:0 with std (blocking mode, like an inherited `--socket-fd` listener)
        /// and releases it as a raw fd owned by the caller.
        fn create_prebound_listener_fd() -> Result<PreboundListener>;

        /// A kj-rs-io address resolving to `127.0.0.1:<port>` for each of `ports`, in order:
        /// a deterministic stand-in for a multi-result DNS lookup, for testing connect()'s
        /// try-each-address fallback.
        fn address_from_loopback_ports(ports: &[u16]) -> Box<TokioAddress>;
    }

    extern "Rust" {
        #[namespace = "kj_rs_io"]
        type TokioAddress = kj_rs_io::TokioAddress;
    }
}
