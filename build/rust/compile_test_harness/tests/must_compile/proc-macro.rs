// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The proc macro resolves through --extern and reads its runtime data while it expands.

pub const DATA: &str = fixture_macro::data_str!();
