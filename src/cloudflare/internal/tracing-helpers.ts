// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import tracing, { type Span } from 'cloudflare-internal:tracing';

export type { Span };

/**
 * Helper function to wrap operations with tracing spans.
 * Automatically handles span lifecycle for both sync and async operations via
 * the underlying `enterSpan` primitive, which also pushes the new span onto
 * the async context so that any spans (or async continuations) created inside
 * `fn` nest correctly beneath it.
 *
 * @param name - The operation name for the span
 * @param fn - The function to execute within the span context
 * @returns The result of the function
 *
 * @example
 * // Synchronous usage
 * const result = withSpan('prepare', (span) => {
 *   span.setAttribute('query', sql);
 *   return new PreparedStatement(sql);
 * });
 *
 * @example
 * // Asynchronous usage
 * const result = await withSpan('exec', async (span) => {
 *   span.setAttribute('query', sql);
 *   return await database.execute(sql);
 * });
 *
 * @note Generator functions are not currently supported and will have their
 * spans ended immediately after the generator object is returned, not when
 * the generator is exhausted.
 */
export function withSpan<T>(name: string, fn: (span: Span) => T): T {
  // `enterSpan` already handles sync vs. promise return (and sync-throw /
  // promise-reject) auto-ending, so this is a pure passthrough.
  return tracing.enterSpan(name, fn);
}
