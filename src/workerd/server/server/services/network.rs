// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The pure parts of the outbound services: the config's `TlsOptions` mapped onto kj-hyper's,
//! and the peer filter a `network` service's `allow` and `deny` lists describe.

use std::net::IpAddr;

use ipnet::IpNet;
use kj_hyper::tls::Keypair;
use kj_hyper::tls::MinVersion;
use kj_hyper::tls::TlsOptions;
use workerd_capnp::tls_options;

use crate::Result;
use crate::config::capnp_error;
use crate::config::text;

/// kj-hyper's TLS options from the config's.
pub fn tls_options(conf: tls_options::Reader<'_>) -> Result<TlsOptions> {
    let keypair = if conf.has_keypair() {
        let keypair = conf.get_keypair().map_err(capnp_error)?;
        Some(Keypair {
            certificate_chain: text(keypair.get_certificate_chain())?,
            private_key: text(keypair.get_private_key())?,
        })
    } else {
        None
    };
    let trusted_certificates = conf
        .get_trusted_certificates()
        .map_err(capnp_error)?
        .iter()
        .map(text)
        .collect::<Result<Vec<_>>>()?;
    // rustls speaks TLS 1.2 and 1.3; every floor the config can name below 1.2 is 1.2.
    let min_version = match conf.get_min_version() {
        Ok(tls_options::Version::Tls1Dot3) => MinVersion::Tls13,
        Ok(_) => MinVersion::Tls12,
        Err(capnp::NotInSchema(_)) => {
            return Err(kj::failed!(
                "Encountered unknown TlsOptions::minVersion setting. Was the config compiled with \
                 a newer version of the schema?"
            ));
        }
    };
    Ok(TlsOptions {
        keypair,
        trusted_certificates,
        require_client_certs: conf.get_require_client_certs(),
        trust_browser_cas: conf.get_trust_browser_cas(),
        min_version,
    })
}

// =======================================================================================
// Peer filter

/// An address range, `address/bits`.
fn cidr(pattern: &str) -> Result<IpNet> {
    pattern
        .parse()
        .map_err(|_| kj::failed!("invalid CIDR: {pattern}"))
}

fn cidrs(patterns: &[&str]) -> Result<Vec<IpNet>> {
    patterns.iter().map(|pattern| cidr(pattern)).collect()
}

/// Whether `range` covers `address`. An IPv4 range also covers the IPv4-mapped IPv6 addresses
/// of its members, as `kj::CidrRange` does.
fn covers(range: &IpNet, address: IpAddr) -> bool {
    range.contains(&address) || range.contains(&address.to_canonical())
}

/// localhost, and 0.0.0.0 / ::, which connect to localhost on many systems.
const LOCAL: &[&str] = &["127.0.0.0/8", "::1/128", "0.0.0.0/32", "::/128"];

/// RFC1918 and RFC4193 private networks, RFC6598 shared address space, link-local ranges.
const PRIVATE: &[&str] = &[
    "10.0.0.0/8",
    "100.64.0.0/10",
    "169.254.0.0/16",
    "172.16.0.0/12",
    "192.168.0.0/16",
    "fc00::/7",
    "fe80::/10",
];

/// Ranges reserved for other protocols: part of neither "public", "private", "network" nor
/// "local", though a config may allow them by CIDR.
const RESERVED: &[&str] = &[
    "192.0.0.0/24",
    "224.0.0.0/4",
    "240.0.0.0/4",
    "255.255.255.255/32",
    "2001::/23",
    "ff00::/8",
];

/// Which peers a `network` service may connect to: `kj::_::NetworkFilter`'s rules over the
/// config's `allow` and `deny` lists.
///
/// An address is allowed when an allow rule covers it and no deny rule at least as specific
/// does; "public" and "network" count as the least specific rules. "unix" and "unix-abstract"
/// are accepted and have no effect: the network client dials IP addresses only.
#[derive(Debug)]
pub struct PeerFilter {
    allow_public: bool,
    allow_network: bool,
    allow: Vec<IpNet>,
    deny: Vec<IpNet>,
    local: Vec<IpNet>,
    private: Vec<IpNet>,
    reserved: Vec<IpNet>,
}

impl PeerFilter {
    pub fn new<'a>(
        allow: impl IntoIterator<Item = &'a str>,
        deny: impl IntoIterator<Item = &'a str>,
    ) -> Result<Self> {
        let mut filter = Self {
            allow_public: false,
            allow_network: false,
            allow: Vec::new(),
            deny: Vec::new(),
            local: cidrs(LOCAL)?,
            private: cidrs(PRIVATE)?,
            reserved: cidrs(RESERVED)?,
        };
        for rule in allow {
            match rule {
                "local" => filter.allow.extend(&filter.local),
                "network" => filter.allow_network = true,
                "private" => {
                    filter.allow.extend(&filter.private);
                    filter.allow.extend(&filter.local);
                }
                "public" => filter.allow_public = true,
                "unix" | "unix-abstract" => {}
                rule => filter.allow.push(cidr(rule)?),
            }
        }
        for rule in deny {
            match rule {
                "local" => filter.deny.extend(&filter.local),
                "network" => {
                    return Err(kj::failed!("don't deny 'network', allow 'local' instead"));
                }
                "private" => filter.deny.extend(&filter.private),
                "public" => {
                    return Err(kj::failed!("don't deny 'public', allow 'private' instead"));
                }
                "unix" | "unix-abstract" => {}
                rule => filter.deny.push(cidr(rule)?),
            }
        }
        Ok(filter)
    }

    #[must_use]
    pub fn allows(&self, address: IpAddr) -> bool {
        let any = |ranges: &[IpNet]| ranges.iter().any(|range| covers(range, address));
        let mut allowed = false;
        let mut allow_specificity = 0;
        if self.allow_public && !any(&self.private) && !any(&self.local) && !any(&self.reserved) {
            allowed = true;
        }
        if self.allow_network && !any(&self.local) && !any(&self.reserved) {
            allowed = true;
        }
        for range in &self.allow {
            if covers(range, address) {
                allow_specificity = allow_specificity.max(range.prefix_len());
                allowed = true;
            }
        }
        allowed
            && !self
                .deny
                .iter()
                .any(|range| covers(range, address) && range.prefix_len() >= allow_specificity)
    }
}

#[cfg(test)]
#[path = "network-test.rs"]
mod tests;
