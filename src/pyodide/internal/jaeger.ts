// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { default as internalJaeger } from 'pyodide-internal:internalJaeger';
import { IS_TRACING } from 'pyodide-internal:metadata';

type JaegerTags = Record<string, string | number | boolean>;

/**
 * Used for tracing via Jaeger.
 *
 * `getTags` runs only when the span is traced, after `callback` returns. For an async callback it
 * runs before the returned promise settles.
 */
export function enterJaegerSpan<T>(
  span: string,
  callback: () => T,
  getTags?: () => JaegerTags
): T {
  if (!IS_TRACING || !internalJaeger.traceId) {
    // Jaeger tracing not enabled or traceId is not present in request.
    return callback();
  }

  return internalJaeger.enterSpan(span, (internalSpan) => {
    const result = callback();
    if (getTags) {
      internalSpan.setTags(getTags());
    }
    return result;
  });
}
