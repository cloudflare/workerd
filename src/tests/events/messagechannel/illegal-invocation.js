// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Every MessagePort member is brand-checked: any other receiver, including
// an object inheriting from MessagePort.prototype, a plain EventTarget, a
// primitive or undefined, throws JSG's illegal-invocation TypeError.

import { throws } from 'node:assert';

const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';

export const membersAreBrandChecked = {
  test() {
    const proto = MessagePort.prototype;
    const { get, set } = Object.getOwnPropertyDescriptor(proto, 'onmessage');
    for (const receiver of [
      {},
      Object.create(proto),
      new EventTarget(),
      new MessageChannel(),
      1,
      'port',
      null,
      undefined,
    ]) {
      for (const call of [
        () => proto.postMessage.call(receiver, 1),
        () => proto.close.call(receiver),
        () => proto.start.call(receiver),
        () => get.call(receiver),
        () => set.call(receiver, null),
      ]) {
        throws(call, { name: 'TypeError', message: kIllegalInvocation });
      }
    }
  },
};
