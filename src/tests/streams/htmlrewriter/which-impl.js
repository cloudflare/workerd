// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { env } from 'cloudflare:workers';

// Distinguishes which streams implementation this worker is running, so
// tests can pin each side of a deliberate divergence exactly.
export const usingTsImpl =
  globalThis.Cloudflare.compatibilityFlags['typescript_implemented_streams'];

// The HTMLRewriter implementation is selected by the TS_HTML_REWRITER
// autogate, which user code cannot read, so each cell says which one it is
// meant to run through its USING_TS_REWRITER binding.
export const usingTsRewriter = env.USING_TS_REWRITER === true;
