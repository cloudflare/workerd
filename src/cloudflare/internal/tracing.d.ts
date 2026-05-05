// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A value acceptable as an attribute on a span.
type SpanValue = string | number | boolean;

declare class Span {
  // Returns true if this span will be recorded to the tracing system. False when the
  // current async context is not being traced, or when the span has already been submitted
  // (which happens automatically when the enterSpan callback returns). Callers can gate
  // expensive attribute-computation code on this.
  readonly isTraced: boolean;

  // Sets a single attribute on the span. If `value` is undefined, the attribute is not set,
  // which is convenient for optional fields.
  setAttribute(key: string, value: SpanValue | undefined): void;
}

// The default export is a singleton instance of the C++ `Tracing` class (see
// `src/workerd/api/tracing.h`). Importers write `import tracing from
// 'cloudflare-internal:tracing'` and then call methods like `tracing.enterSpan(...)` on
// the instance. The runtime wires this up via `addBuiltinModule<Tracing>` in
// `registerTracingModule`.
declare const tracing: {
  // Creates a new child span of the current span, pushes it onto the async context as
  // the active span, invokes `callback(span, ...args)`, and automatically ends the span
  // when the callback returns (sync) or when its returned promise settles (async, either
  // fulfilled or rejected). If no IO context is present the callback runs with a no-op
  // span.
  enterSpan<T, A extends unknown[]>(
    name: string,
    callback: (span: Span, ...args: A) => T,
    ...args: A
  ): T;

  // The `Span` class is exposed as a nested type so callers can reference the type via
  // `InstanceType<typeof tracing.Span>` (see `tracing-helpers.ts`).
  readonly Span: typeof Span;
};
export default tracing;

// Re-export `Span` as a named type export for callers that prefer `import type { Span }`
// over `InstanceType<typeof tracing.Span>`. The runtime module does not have a named
// `Span` export - this is purely a type-level convenience.
export type { Span };
