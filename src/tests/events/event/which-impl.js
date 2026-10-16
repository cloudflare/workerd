// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Distinguishes which events implementation this worker is running, and
// whether the C++ one runs under pedantic_wpt, so tests can pin each side
// of a deliberate divergence exactly.

const flags = globalThis.Cloudflare.compatibilityFlags;

export const usingTsImpl = flags['typescript_implemented_events'];
export const pedanticWpt = flags['pedantic_wpt'];
