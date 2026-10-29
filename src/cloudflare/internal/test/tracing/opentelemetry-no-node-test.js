// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

const api = globalThis[Symbol.for('opentelemetry.js.api.1')];
if (api?.version !== '1.10.0')
  throw new Error('registration must precede module evaluation');
const spanKey = Symbol.for('OpenTelemetry Context Key SPAN');
export const withoutNodeCompatibility = {
  async test() {
    const tracer = api.trace.getTracer('without-node');
    await tracer.startActiveSpan('otel-no-node', async (span) => {
      const ctx = api.context.active();
      if (ctx.getValue(spanKey) !== span || !span.isRecording())
        throw new Error('active span');
      await new Promise((resolve) => setTimeout(resolve, 1));
      await new Promise((resolve) => queueMicrotask(resolve));
      if (api.context.active() !== ctx)
        throw new Error('context lost across async work');
      tracer.startSpan('otel-no-node-child').end();
      span.end();
    });
  },
};
