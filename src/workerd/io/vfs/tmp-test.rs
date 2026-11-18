// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;
use crate::tests::TestHost;

#[test]
fn one_scope_per_thread() {
    assert!(TmpDirStoreScope::current().is_none());
    {
        let scope = TmpDirStoreScope::enter().unwrap();
        assert!(TmpDirStoreScope::current().unwrap().is_same(scope.store()));
        assert!(TmpDirStoreScope::enter().is_none());
        // The failed attempt left the current scope in place.
        assert!(TmpDirStoreScope::current().unwrap().is_same(scope.store()));
    }
    assert!(TmpDirStoreScope::current().is_none());
    // Stores made directly are independent of the scope.
    assert!(!TmpDirStore::new().is_same(&TmpDirStore::new()));
}

#[test]
fn the_request_store_comes_before_the_scope() {
    let scope = TmpDirStoreScope::enter().unwrap();
    let host = TestHost::new();
    let request_store = host.tmp_store.borrow().clone().unwrap();
    assert!(current_tmp_store(&host).unwrap().is_same(&request_store));
    let outside = TestHost::outside_request();
    assert!(current_tmp_store(&outside).unwrap().is_same(scope.store()));
    drop(scope);
    assert!(current_tmp_store(&outside).is_none());
}
