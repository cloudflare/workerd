// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn port_is_mysqls_for_mysql_and_postgress_otherwise() {
    assert_eq!(port_for_scheme("mysql"), 3306);
    assert_eq!(port_for_scheme("postgresql"), 5432);
    assert_eq!(port_for_scheme("postgres"), 5432);
    assert_eq!(port_for_scheme(""), 5432);
}

#[test]
fn random_host_is_lowercase_hex() {
    let random = [0x00, 0x01, 0xab, 0xff];
    assert_eq!(random_host(&random), "0001abff.hyperdrive.local");
}

#[test]
fn synthetic_ip_holds_the_suffix_in_its_low_bits() {
    assert_eq!(synthetic_ip(0), "240.0.0.0");
    assert_eq!(synthetic_ip(0x0102), "240.0.1.2");
    assert_eq!(synthetic_ip(u16::MAX), "240.0.255.255");
}

#[test]
fn find_free_ip_walks_from_start_and_wraps() {
    let taken = ["240.0.255.255", "240.0.0.0"];
    let mut asked = Vec::new();
    let ip = find_free_ip(u16::MAX, |candidate| {
        asked.push(candidate.to_owned());
        Ok(taken.contains(&candidate))
    })
    .unwrap();
    assert_eq!(ip, "240.0.0.1");
    assert_eq!(asked, ["240.0.255.255", "240.0.0.0", "240.0.0.1"]);
}

#[test]
fn find_free_ip_fails_once_every_address_is_taken() {
    let mut asked = 0_u32;
    let err = find_free_ip(7, |_| {
        asked += 1;
        Ok(true)
    })
    .unwrap_err();
    assert_eq!(asked, 1 << 16);
    assert_eq!(err.message, "no free Hyperdrive address available");
}

#[test]
fn connection_string_disables_tls_as_each_database_spells_it() {
    assert_eq!(
        connection_string("postgresql", "u", "p", "h.hyperdrive.local", "db"),
        "postgresql://u:p@h.hyperdrive.local:5432/db?sslmode=disable"
    );
    assert_eq!(
        connection_string("mysql", "u", "p", "h.hyperdrive.local", "db"),
        "mysql://u:p@h.hyperdrive.local:3306/db?ssl-mode=disabled"
    );
}
