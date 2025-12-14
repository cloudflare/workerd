// TODO: figure out how to make bazel resolve env.d.ts
// eslint-disable-next-line
// @ts-ignore
import innerEnv from 'cloudflare-internal:env';

// A generator that keeps the environment patched until it is resolved.
// Used to define the patch_env context manager, see
// src/pyodide/internal/workers-api/src/workers/_workers.py
export function* patch_env_helper(patch: unknown): Generator<void> {
  // eslint-disable-next-line
  using _ = innerEnv.pythonPatchEnv(patch);
  yield;
}
