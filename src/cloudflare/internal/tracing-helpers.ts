// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import tracing, {
  type Exception,
  type Span,
} from 'cloudflare-internal:tracing';

export type { Span };

/**
 * Helper function to wrap operations with tracing spans.
 * Automatically handles span lifecycle for both sync and async operations via
 * the underlying `enterSpan` primitive, which also pushes the new span onto
 * the async context so that any spans (or async continuations) created inside
 * `fn` nest correctly beneath it.
 *
 * If `fn` throws or its returned promise rejects, the error is recorded on the
 * span (see `recordError`) and then propagates unchanged.
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
  // `enterSpan` ends the span when `fn` returns or throws, or when its returned
  // promise settles, so the error is recorded before the span ends.
  return tracing.enterSpan(name, (span) => {
    let result: T;
    try {
      result = fn(span);
    } catch (error) {
      recordError(span, error);
      throw error;
    }
    if (result instanceof Promise) {
      return result.catch((error: unknown) => {
        recordError(span, error);
        throw error;
      }) as T;
    }
    return result;
  });
}

/**
 * Marks the operation covered by `span` as failed: records `error` as an
 * exception, then sets the status to error, described by the error's message.
 */
function recordError(span: Span, error: unknown): void {
  try {
    if (
      typeof error === 'string' ||
      (typeof error === 'object' && error !== null)
    ) {
      span.recordException(error as Exception);
    }
    const message =
      typeof error === 'object' && error !== null && 'message' in error
        ? String(error.message)
        : String(error);
    span.setStatus({ code: 'error', message });
  } catch {
    // Reading the error's properties can run user code. Never replace the
    // operation's own error.
  }
}

/**
 * Helper function to start a span that is active while `fn` runs, but whose
 * lifecycle is controlled explicitly by the caller via `span.end()`.
 *
 * @param name - The operation name for the span
 * @param fn - The function to execute while the span is active
 * @returns The result of the function
 *
 * @example
 * // Explicit lifecycle usage for stream-drain instrumentation
 * const stream = startActiveSpan('stream', (span) => {
 *   return body.pipeThrough(new TransformStream({
 *     flush() {
 *       span.setAttribute('phase.drained', true);
 *       span.end();
 *     },
 *   }));
 * });
 */
export function startActiveSpan<T>(name: string, fn: (span: Span) => T): T {
  return tracing.startActiveSpan(name, fn);
}
