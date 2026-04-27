// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// NOTE: Constants in this file must match `workerd/util/exception.h`

/// If an exception is thrown for exceeding CPU time limits, it will contain this detail.
pub const CPU_LIMIT_DETAIL_ID: u64 = 0xfdcb_787b_a424_0576;

/// If an exception is thrown for exceeding memory limits, it will contain this detail.
pub const MEMORY_LIMIT_DETAIL_ID: u64 = 0xbaf7_6dd7_ce5b_d8cf;

/// If an exception is thrown for worker killed before start, it will contain this detail.
pub const SCRIPT_KILLED_DETAIL_ID: u64 = 0xf893_5d57_9c20_da70;
