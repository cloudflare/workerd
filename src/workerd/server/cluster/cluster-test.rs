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

/// Each probe write's outcome unknown: what the object holds says which
/// landed, so a store that keeps preconditions passes and one that ignores
/// them does not.
#[test]
fn probe_writes_of_unknown_outcome_are_settled_by_reading_the_probe() {
    block_on(async {
        let check = |store: Memory| async {
            store
                .unknown
                .borrow_mut()
                .extend(["n.probe"; 4].map(str::to_owned));
            let unknown = store.unknown.clone();
            let checked = check_conditional_writes(&Bucket::new(store, ""), "n.probe").await;
            assert!(unknown.borrow().is_empty());
            checked
        };
        check(Memory::default()).await.unwrap();
        let lax = Memory::default();
        lax.ignores_guards.set(true);
        let Err(Error::Config(message)) = check(lax).await else {
            panic!("a store that ignores preconditions passed");
        };
        assert!(message.starts_with("the bucket ignores conditional writes"));
        let weak = Memory::default();
        weak.never_matches.set(true);
        let Err(Error::Config(message)) = check(weak).await else {
            panic!("a store whose versions never match passed");
        };
        assert!(message.starts_with("the bucket refused a conditional write whose condition held"));
    });
}
