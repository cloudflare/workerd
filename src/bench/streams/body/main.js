// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the body benchmarks. Explicit named re-exports only (a
// collision must be a load-time error, not a silently dropped group). Each
// named export is a group of the report; the default export is the fetch
// handler that the SELF binding calls (see service-binding.js).

export { default } from 'service-binding';
export { consume } from 'consume';
export { read } from 'read';
export { pipe } from 'pipe';
export { clone } from 'clone';
export { serviceBinding } from 'service-binding';
export { htmlRewriter } from 'html-rewriter';
export { formData } from 'form-data';
