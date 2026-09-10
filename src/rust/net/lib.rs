// Production code must not panic; test code is exempt via clippy.toml allow-*-in-tests.
#![deny(clippy::expect_used, clippy::panic, clippy::unreachable)]
#![deny(clippy::todo, clippy::unimplemented)]

use std::net::IpAddr;
use std::str::FromStr;

#[cxx::bridge(namespace = "workerd::rust::net")]
mod ffi {
    extern "Rust" {
        fn canonicalize_ip(input: &str) -> String;
    }
}

#[must_use]
pub fn canonicalize_ip(input: &str) -> String {
    IpAddr::from_str(input).map_or(String::new(), |ip| ip.to_string())
}
