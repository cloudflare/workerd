// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use capnp::message::Builder;

use super::*;

fn ip(text: &str) -> IpAddr {
    text.parse().unwrap()
}

#[test]
fn tls_options_map_onto_kj_hyper() {
    let mut message = Builder::new_default();
    let mut conf = message.init_root::<tls_options::Builder<'_>>();
    conf.set_trust_browser_cas(true);
    conf.set_require_client_certs(true);
    conf.set_min_version(tls_options::Version::Tls1Dot3);
    conf.reborrow().init_trusted_certificates(1).set(0, "cert");
    let mut keypair = conf.reborrow().init_keypair();
    keypair.set_private_key("key");
    keypair.set_certificate_chain("chain");
    let options = tls_options(message.get_root_as_reader().unwrap()).unwrap();
    assert!(options.trust_browser_cas);
    assert!(options.require_client_certs);
    assert_eq!(options.min_version, MinVersion::Tls13);
    assert_eq!(options.trusted_certificates, ["cert"]);
    let keypair = options.keypair.unwrap();
    assert_eq!(
        (
            keypair.private_key.as_str(),
            keypair.certificate_chain.as_str()
        ),
        ("key", "chain")
    );
    // rustls speaks no TLS below 1.2.
    let mut conf = message.get_root::<tls_options::Builder<'_>>().unwrap();
    conf.set_min_version(tls_options::Version::Tls1Dot0);
    let options = tls_options(message.get_root_as_reader().unwrap()).unwrap();
    assert_eq!(options.min_version, MinVersion::Tls12);
}

#[test]
fn public_excludes_private_local_and_reserved() {
    let filter = PeerFilter::new(["public"], []).unwrap();
    assert!(filter.allows(ip("1.1.1.1")));
    assert!(filter.allows(ip("2606:4700::1111")));
    assert!(!filter.allows(ip("10.0.0.1")));
    assert!(!filter.allows(ip("127.0.0.1")));
    assert!(!filter.allows(ip("0.0.0.0")));
    assert!(!filter.allows(ip("224.0.0.1")));
    assert!(!filter.allows(ip("fe80::1")));
    assert!(!filter.allows(ip("::ffff:192.168.1.1")));
}

#[test]
fn private_includes_local_and_network_excludes_it() {
    let private = PeerFilter::new(["private"], []).unwrap();
    assert!(private.allows(ip("10.0.0.1")));
    assert!(private.allows(ip("127.0.0.1")));
    assert!(!private.allows(ip("1.1.1.1")));
    let network = PeerFilter::new(["network"], []).unwrap();
    assert!(network.allows(ip("10.0.0.1")));
    assert!(network.allows(ip("1.1.1.1")));
    assert!(!network.allows(ip("127.0.0.1")));
    assert!(!network.allows(ip("255.255.255.255")));
}

#[test]
fn a_deny_wins_unless_a_more_specific_allow_covers_the_address() {
    let filter = PeerFilter::new(["private", "10.1.0.0/16"], ["10.0.0.0/8", "local"]).unwrap();
    assert!(!filter.allows(ip("10.2.3.4")));
    assert!(filter.allows(ip("10.1.3.4")));
    assert!(filter.allows(ip("192.168.0.1")));
    assert!(!filter.allows(ip("127.0.0.1")));
    let public = PeerFilter::new(["public"], ["1.1.1.0/24"]).unwrap();
    assert!(!public.allows(ip("1.1.1.1")));
    assert!(public.allows(ip("1.0.0.1")));
}

#[test]
fn denying_network_or_public_is_refused_and_unix_rules_are_ignored() {
    assert!(PeerFilter::new(["public"], ["network"]).is_err());
    assert!(PeerFilter::new(["network"], ["public"]).is_err());
    assert!(PeerFilter::new(["10.0.0.0/33"], []).is_err());
    assert!(PeerFilter::new([], ["10.0.0.0"]).is_err());
    let filter = PeerFilter::new(["unix", "local"], ["unix-abstract"]).unwrap();
    assert!(filter.allows(ip("127.0.0.1")));
    assert!(!filter.allows(ip("1.1.1.1")));
}
