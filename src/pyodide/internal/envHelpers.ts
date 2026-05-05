// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import innerEnv from 'cloudflare-internal:env';

// A generator that keeps the environment patched until it is resolved.
// Used to define the patch_env context manager, see
// src/pyodide/internal/workers-api/src/workers/_workers.py
export function* patch_env_helper(patch: unknown): Generator<void> {
  using _ = innerEnv.pythonPatchEnv(patch);
  yield;
}
