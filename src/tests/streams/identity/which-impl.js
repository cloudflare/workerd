// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Distinguishes which streams implementation this worker is running. The
// suite runs the same test modules under both configs; where the two
// implementations deliberately diverge, tests use this to assert the exact
// behavior of each implementation rather than loosening the assertion to
// whatever both happen to satisfy. A divergence is thereby pinned: if either
// implementation changes its side of it, the corresponding cell fails.
export const usingTsImpl =
  globalThis.Cloudflare.compatibilityFlags['typescript_implemented_streams'];
