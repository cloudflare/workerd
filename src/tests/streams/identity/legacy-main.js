// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the legacy (unflagged) identity streams suite — the
// regression guard for behaviors that workers predating the pinned
// compatibility flags still depend on. Runs only against the C++
// implementation; see identity-cpp-legacy.wd-test for the rationale.
// Re-exports are explicit for the same reason as main.js: a collision must
// be a load-time SyntaxError, not a silently dropped test.

export { legacyPropertyPlacement, legacyToStringTag } from 'legacy-api-surface';

export { legacyInvalidChunkThrowsSynchronously } from 'legacy-invalid-chunks';

export {
  legacyByobFillsInPlace,
  legacyByobEofReturnsUndefined,
} from 'legacy-byob';

export {
  legacyAbortWaitsForPendingWrite,
  legacyAbortWithPendingReadResolves,
} from 'legacy-abort';
