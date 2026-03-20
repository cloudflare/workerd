// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Get the current environment, if any
export function getCurrentEnv(): Record<string, unknown> | undefined;
export function getCurrentExports(): Record<string, unknown> | undefined;
export function withEnv(newEnv: unknown, fn: () => unknown): unknown;
export function withExports(newExports: unknown, fn: () => unknown): unknown;
export function withEnvAndExports(
  newEnv: unknown,
  newExports: unknown,
  fn: () => unknown
): unknown;
