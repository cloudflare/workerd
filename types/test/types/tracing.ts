// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';
import { tracing } from 'cloudflare:workers';

const span = tracing.startSpan('linked');
const link: TracingSpanLink = {
  context: {
    ...span.spanContext(),
    isRemote: false,
    traceState: 'vendor=state',
  },
  attributes: {
    message: 'work',
    retries: 2,
    successful: true,
    absent: undefined,
  },
  droppedAttributesCount: 3,
};
expectTypeOf(span.addLink(link)).toEqualTypeOf<typeof span>();
expectTypeOf(span.addLinks([link] as const)).toEqualTypeOf<typeof span>();
// @ts-expect-error links require a target context
span.addLink({ attributes: { message: 'missing context' } });
// @ts-expect-error span creation options are not part of the runtime API
tracing.startSpan('linked', { links: [link] });

const handler: TailStream.TailEventHandlerObject = {
  spanLinks(event) {
    expectTypeOf(event.event.type).toEqualTypeOf<'spanLinks'>();
    expectTypeOf(event.event.links[0].context.isRemote).toEqualTypeOf<
      boolean | undefined
    >();
    expectTypeOf(
      event.event.links[0].droppedAttributesCount
    ).toEqualTypeOf<number>();
    // @ts-expect-error event links are immutable
    event.event.links.push(event.event.links[0]);
  },
};
void handler;
