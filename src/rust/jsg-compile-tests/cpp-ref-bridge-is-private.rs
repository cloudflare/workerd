// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The bridge's functions change a `jsg_cpp::Ref`'s reference count without checking the isolate
// lock, so only `jsg_cpp::Ref`, which takes the lock, calls them.
use jsg_cpp::bridge; //~ ERROR E0432
