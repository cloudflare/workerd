// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Distinguishes which structuredClone implementation this worker is running,
// so tests can pin each side of a deliberate divergence exactly.
export const usingTsImpl =
  globalThis.Cloudflare.compatibilityFlags[
    'typescript_implemented_structured_clone'
  ];
