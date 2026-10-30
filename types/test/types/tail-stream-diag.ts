// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';

// The runtime dispatches `StreamDiagnostic` events to the handler-object key
// `streamDiagEvent` (getHandlerName in src/workerd/io/trace-stream.c++), so
// the object form of a tailStream() handler must accept that key.
expectTypeOf<
  TailStream.TailEventHandlerObject['streamDiagEvent']
>().toEqualTypeOf<
  TailStream.TailEventHandler<TailStream.StreamDiagnostic> | undefined
>();

export default {
  tailStream(event) {
    expectTypeOf(event.event).toEqualTypeOf<TailStream.Onset>();
    return {
      streamDiagEvent(ev) {
        expectTypeOf(ev).toEqualTypeOf<
          TailStream.TailEvent<TailStream.StreamDiagnostic>
        >();
        expectTypeOf(ev.event.type).toEqualTypeOf<'streamDiagnostic'>();
        expectTypeOf(
          ev.event.diagnostic.diagnosticsType
        ).toEqualTypeOf<'droppedEvents'>();
        expectTypeOf(ev.event.diagnostic.count).toEqualTypeOf<number>();
      },
      outcome(ev) {
        expectTypeOf(ev.event).toEqualTypeOf<TailStream.Outcome>();
      },
    };
  },
} satisfies ExportedHandler;
