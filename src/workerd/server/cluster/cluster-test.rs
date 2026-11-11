// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use futures::executor::block_on;

use super::check_conditional_writes;
use crate::Bucket;
use crate::Error;
use crate::bucket::memory::Memory;

#[test]
fn a_bucket_that_ignores_preconditions_is_refused() {
    block_on(async {
        assert!(
            check_conditional_writes(&Bucket::memory(""), "n.probe")
                .await
                .is_ok()
        );
        let message = |store: Memory| async {
            let bucket = Bucket::new(store, "");
            match check_conditional_writes(&bucket, "n.probe").await {
                Err(Error::Config(message)) => message,
                other => panic!("expected a config error, not {other:?}"),
            }
        };
        let lax = Memory::default();
        lax.ignores_guards.set(true);
        assert!(
            message(lax)
                .await
                .starts_with("the bucket ignores conditional writes")
        );
        let weak = Memory::default();
        weak.never_matches.set(true);
        assert!(
            message(weak)
                .await
                .starts_with("the bucket refused a conditional write whose condition held")
        );
    });
}
