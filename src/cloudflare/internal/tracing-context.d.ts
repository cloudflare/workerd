// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import type { TracingSpanContext } from './tracing';

// Private native primitives for the built-in OTel adapter, not a public Workers API.
declare const tracingContext: {
  withActiveSpanContext<T>(
    context: TracingSpanContext | undefined,
    callback: () => T
  ): T;
  createAsyncContextVariable(): {
    get(): unknown;
    run<T>(value: unknown, callback: () => T): T;
  };
};
export default tracingContext;
