// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A value acceptable as an attribute on a span.
type SpanValue = string | number | boolean;

export class Span {
  // Returns true if this span will be recorded to the tracing system. False when the
  // current async context is not being traced, or when the span has already been submitted
  // (which happens automatically when the enterSpan callback returns). Callers can gate
  // expensive attribute-computation code on this.
  readonly isTraced: boolean;

  // Sets a single attribute on the span. If `value` is undefined, the attribute is not set,
  // which is convenient for optional fields.
  setAttribute(key: string, value: SpanValue | undefined): void;
}

// Creates a new child span of the current span, pushes it onto the async context as the
// active span, invokes callback(span, ...args), and automatically ends the span when the
// callback returns (sync) or when its returned promise settles (async, either fulfilled or
// rejected). If no IO context is present the callback runs with a no-op span.
function enterSpan<T, A extends unknown[]>(
  name: string,
  callback: (span: Span) => T,
  ...args: A
): T;
