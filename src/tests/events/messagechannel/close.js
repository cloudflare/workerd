// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// close() closes both ports of the pair, firing a trusted 'close' event on
// each, the peer's first. Its dispatch always propagates a listener's
// exception out of close(), whatever spec_compliant_dispatch_exceptions
// says.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { mock } from 'node:test';

export const closeFiresOnBothPortsPeerFirst = {
  test() {
    const { port1, port2 } = new MessageChannel();
    const order = [];
    for (const [name, port] of [
      ['port1', port1],
      ['port2', port2],
    ]) {
      port.addEventListener('close', (event) => {
        strictEqual(event.isTrusted, true);
        strictEqual(event.bubbles, false);
        strictEqual(event.cancelable, false);
        strictEqual(event.target, port);
        order.push(name);
      });
    }
    port1.close();
    deepStrictEqual(order, ['port2', 'port1']);
    port1.close();
    port2.close();
    deepStrictEqual(order, ['port2', 'port1']);
  },
};

// onclose is not an event handler attribute, but dispatch still calls a
// function stored in a plain onclose property.
export const oncloseProperty = {
  test() {
    const { port1, port2 } = new MessageChannel();
    const closeHandler = mock.fn();
    port1.onclose = closeHandler;
    port2.onclose = closeHandler;
    port1.close();
    strictEqual(closeHandler.mock.callCount(), 2);
  },
};

export const throwingCloseListenerEscapesClose = {
  test() {
    const { port1, port2 } = new MessageChannel();
    const boom = new Error('close boom');
    port2.addEventListener('close', () => {
      throw boom;
    });
    const port1Closed = mock.fn();
    port1.addEventListener('close', port1Closed);
    // The peer's listener throws, so this port's event never fires, though
    // both ports are closed.
    throws(
      () => port1.close(),
      (e) => e === boom
    );
    port1.close();
    port2.close();
    strictEqual(port1Closed.mock.callCount(), 0);
  },
};
