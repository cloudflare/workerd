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
