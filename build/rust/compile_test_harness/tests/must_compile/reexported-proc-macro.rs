// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A proc macro that a dependency re-exports reads its runtime data while it expands.

pub const DATA: &str = fixture::data_str!();
