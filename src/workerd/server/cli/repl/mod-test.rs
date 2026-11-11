// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn loads_the_command_line_config_into_s() {
    assert_eq!(
        load_config_input("config.capnp", None),
        "s = await workerd.loadConfig(\"config.capnp\")"
    );
    assert_eq!(
        load_config_input("it's \"q\".capnp", Some("dev")),
        "s = await workerd.loadConfig(\"it's \\\"q\\\".capnp\", { constName: \"dev\" })"
    );
}
