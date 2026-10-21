// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// One error, annotated twice.

pub fn f(_a: Undefined) {} //~ ERROR E0425
//~| ERROR E0425
