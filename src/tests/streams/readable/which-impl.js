// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Distinguishes which streams implementation this worker is running, so
// tests can pin each side of a deliberate divergence exactly.
export const usingTsImpl =
  globalThis.Cloudflare.compatibilityFlags['typescript_implemented_streams'];

// True in the transform-cpp-pedantic cell: the dateless opt-in pedantic_wpt
// flag aligns the C++ finish-operation coordination (abort/close/cancel
// races) with the spec.
export const pedanticWpt =
  globalThis.Cloudflare.compatibilityFlags['pedantic_wpt'];
