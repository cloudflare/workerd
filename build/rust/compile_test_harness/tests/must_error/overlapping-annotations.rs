// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Both errors match the first annotation, but only the first error matches the second, so the
// first annotation must take the second error.

pub fn f(_a: Undefined, _b: AlsoUndefined) {}
//~^ ERROR E0425
//~| ERROR E0425: `Undefined`
