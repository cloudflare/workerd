// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A top level that throws. The zygote evaluates it, fails and makes no snapshot; the Worker
// evaluates it again and its own failure is the one reported.
console.log('MARK throws log');
throw new Error('MARK throws error');
