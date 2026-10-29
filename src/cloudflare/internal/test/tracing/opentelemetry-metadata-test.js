// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { trace } from '@opentelemetry/api';
import { tracing } from 'cloudflare:workers';

export const metadata = {
  test(_controller, env) {
    // The API's convenience getTracer(name, version) drops a third argument; the provider
    // interface accepts TracerOptions, including schemaUrl.
    const provider = trace.getTracerProvider();
    const tracer = provider.getTracer('metadata-test', '1.2.3', {
      schemaUrl: 'https://example.com/schema',
    });
    for (let kind = 0; kind <= 4; kind++) {
      const span = tracer.startSpan(`kind-${kind}`, { kind });
      assert.strictEqual(span.isRecording(), env.enabled);
      span.updateName(`renamed-kind-${kind}`);
      span.end();
    }
    tracer.startSpan('default-kind').end();
    trace.getTracer('name-only').startSpan('name-only').end();
    trace.getTracer('metadata-test', '2.0').startSpan('other-version').end();
    provider
      .getTracer('empty', '', { schemaUrl: '' })
      .startSpan('empty-optionals')
      .end();
    // Extra arguments to the public API remain ignored, including while the flag is off.
    tracing
      .startSpan(`native-${env.enabled}`, {
        kind: 4,
        scope: { name: 'ignored' },
      })
      .end();
  },
};
