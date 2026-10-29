// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import type { Span, TracingSpanContext } from './tracing';

// Private native primitives for the built-in OTel adapter, not a public Workers API.
declare const tracingContext: {
  addLink(
    span: Span,
    context: TracingSpanContext,
    attributes: Record<string, unknown>,
    isRemote: boolean,
    traceState?: string
  ): void;
  addEvent(
    span: Span,
    name: string,
    attributes: Record<string, unknown>,
    time?: {
      seconds: number;
      nanoseconds: number;
    }
  ): void;
  startSpan(
    name: string,
    options: {
      kind: number;
      scope: { name: string; version?: string; schemaUrl?: string };
    }
  ): Span;
  getSpanCreationMode(): 'untraced' | 'unsampled' | 'recording';
  getInvocationSpanContext(): TracingSpanContext | undefined;
  withActiveSpanContext<T>(
    context: TracingSpanContext | undefined,
    callback: () => T,
    suppress?: boolean
  ): T;
  createAsyncContextVariable(): {
    get(): unknown;
    run<T>(value: unknown, callback: () => T): T;
  };
};
export default tracingContext;
