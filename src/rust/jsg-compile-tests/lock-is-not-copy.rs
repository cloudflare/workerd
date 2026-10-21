// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A lock cannot be duplicated.
pub fn t(lock: &mut jsg::Lock<'_>) {
    let _copy = *lock; //~ ERROR E0507
}
