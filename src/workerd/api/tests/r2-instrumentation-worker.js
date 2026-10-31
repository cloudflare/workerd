// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Only export the suites whose spans are checked by r2-instrumentation-test.js. The other shared
// R2 suites are exercised by the HTTP and JSRPC transport tests without instrumentation.
export {
  default,
  deletePerKeyErrorParityTests,
  r2ValidationTests,
} from './r2-test.js';
