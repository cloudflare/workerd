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
    IpAddr::from_str(input)
        .map(|ip| ip.to_string())
        .unwrap_or(String::new())
}
