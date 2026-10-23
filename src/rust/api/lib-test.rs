// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_test::Harness;

use super::*;

#[test]
fn test_wrap_resource_equality() {
    let harness = Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let dns_util = DnsUtil::new();

        let lhs = dns_util.clone().to_js(lock);
        let rhs = dns_util.to_js(lock);

        assert_eq!(lhs, rhs);
        Ok(())
    });
}
