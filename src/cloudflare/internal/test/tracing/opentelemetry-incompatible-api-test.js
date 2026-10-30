// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import api, {
  trace,
  context,
  diag,
  isSpanContextValid,
} from '@opentelemetry/api';
import { tracing } from 'cloudflare:workers';

export const incompatibleCopy = {
  test() {
    const key = Symbol.for('opentelemetry.js.api.1');
    const registration = globalThis[key];
    const registrationKeys = Reflect.ownKeys(registration);
    assert.strictEqual(registration.version, '1.10.0');
    assert.strictEqual(api.trace, trace);
    assert.strictEqual(api.context, context);
    assert.ok(isSpanContextValid(tracing.getActiveSpan().spanContext()));
    // The simulated 1.11 copy cannot read the 1.10 runtime registration.
    assert.strictEqual(trace.getActiveSpan(), undefined);
    const span = trace
      .getTracer('incompatible-api')
      .startSpan('must-not-record');
    assert.strictEqual(span.isRecording(), false);
    assert.strictEqual(isSpanContextValid(span.spanContext()), false);
    span.end();
    // Unlike the duplicate trace-provider check, this exercises the registration version gate
    // for an API the runtime has not registered.
    assert.strictEqual(Object.hasOwn(registration, 'diag'), false);
    const logger = Object.fromEntries(
      ['error', 'warn', 'info', 'debug', 'verbose'].map((level) => [
        level,
        () => {},
      ])
    );
    assert.strictEqual(diag.setLogger(logger), false);
    assert.strictEqual(
      trace.setGlobalTracerProvider({
        getTracer() {
          throw new Error('incompatible provider must not replace runtime');
        },
      }),
      false
    );
    assert.strictEqual(globalThis[key], registration);
    assert.deepStrictEqual(Reflect.ownKeys(registration), registrationKeys);
    const compatible = registration.trace
      .getTracer('runtime-still-registered')
      .startSpan('compatible-span');
    assert.strictEqual(compatible.isRecording(), true);
    assert.ok(isSpanContextValid(compatible.spanContext()));
    compatible.end();
    assert.strictEqual(
      globalThis[Symbol.for('opentelemetry.js.api.2')],
      undefined
    );
  },
};
