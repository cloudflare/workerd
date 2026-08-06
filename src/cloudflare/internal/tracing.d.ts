// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A value acceptable as an attribute on a span.
type SpanValue = string | number | boolean;

interface SpanAttributes {
  [key: string]: SpanValue | undefined;
}

interface ExceptionWithCode {
  code: string | number;
  name?: string;
  message?: string;
  stack?: string;
}

interface ExceptionWithMessage {
  code?: string | number;
  message: string;
  name?: string;
  stack?: string;
}

interface ExceptionWithName {
  code?: string | number;
  message?: string;
  name: string;
  stack?: string;
}

type Exception =
  ExceptionWithCode | ExceptionWithMessage | ExceptionWithName | string;

declare class Span {
  // Returns true if this span will be recorded to the tracing system. False when the
  // current async context is not being traced, or when the span has already been submitted.
  // Callers can gate expensive attribute-computation code on this.
  readonly isTraced: boolean;

  // Sets a single attribute on the span.
  setAttribute(key: string, value: SpanValue): this;

  // Sets multiple attributes on the span. Attributes with undefined values are ignored.
  setAttributes(attributes: SpanAttributes): this;

  // Records an exception event on the span. Calls after the span has ended are ignored.
  recordException(exception: Exception): void;

  // Ends the span and submits its attributes to the tracing system. Idempotent. This is a no-op
  // for the invocation span returned by getActiveSpan(), whose lifecycle is owned by the runtime.
  end(): void;
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

  // Creates a span, makes it active while invoking `callback(span, ...args)`, and
  // returns the callback result without automatically ending the span. Callers must
  // invoke `span.end()` explicitly.
  startActiveSpan<T, A extends unknown[]>(
    name: string,
    callback: (span: Span, ...args: A) => T,
    ...args: A
  ): T;

  // Creates a span as a child of the current active user tracing span without making it active.
  // Callers must invoke `span.end()` explicitly.
  startSpan(name: string): Span;

  // Returns the span associated with the current async context, or the invocation span when no
  // user-created span is active. Returns undefined outside an invocation or when execution is
  // detached into the root async context.
  getActiveSpan(): Span | undefined;

  // The `Span` class is exposed as a nested type so callers can reference the type via
  // `InstanceType<typeof tracing.Span>` (see `tracing-helpers.ts`).
  readonly Span: typeof Span;
};
export default tracing;

// Re-export `Span` as a named type export for callers that prefer `import type { Span }`
// over `InstanceType<typeof tracing.Span>`. The runtime module does not have a named
// `Span` export - this is purely a type-level convenience.
export type { Exception, Span };
