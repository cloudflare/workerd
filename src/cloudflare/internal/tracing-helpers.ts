// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import tracing from 'cloudflare-internal:tracing';

/**
 * Helper function to wrap operations with tracing spans.
 * Automatically handles span lifecycle for both sync and async operations.
 *
 * @param name - The operation name for the span
 * @param fn - The function to execute within the span context
 * @returns The result of the function
 *
 * @example
 * // Synchronous usage
 * const result = withSpan('prepare', (span) => {
 *   span.setTag('query', sql);
 *   return new PreparedStatement(sql);
 * });
 *
 * @example
 * // Asynchronous usage
 * const result = await withSpan('exec', async (span) => {
 *   span.setTag('query', sql);
 *   return await database.execute(sql);
 * });
 */
export function withSpan<T>(
  name: string,
  fn: (span: ReturnType<typeof tracing.startSpan>) => T
): T {
  const span = tracing.startSpan(name);

  // Check if this is a recording span (not a no-op)
  const isRecording = span.getIsRecording?.() ?? true;

  try {
    const result = fn(span);

    // Only end recording spans, not no-op spans
    if (isRecording) {
      // Handle promises - ensure span ends after async completion
      if (result && typeof result === 'object' && 'then' in result) {
        return (result as unknown as Promise<any>).finally(() => {
          span.end();
        }) as T;
      }

      // Synchronous result - end span immediately
      span.end();
    }

    return result;
  } catch (error) {
    // Ensure span ends on error (only for recording spans)
    if (isRecording) {
      span.end();
    }
    throw error;
  }
}
