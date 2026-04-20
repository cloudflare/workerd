// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

import EventEmitter, { init } from 'node:events';

export const eventsInitTest = {
  test() {
    assert.strictEqual(init, EventEmitter.init);

    // init sets up the listener state of a pre-ES6-style subclass instance.
    function Legacy() {
      init.call(this);
    }
    Object.setPrototypeOf(Legacy.prototype, EventEmitter.prototype);
    const emitter = new Legacy();
    let received;
    emitter.on('ping', (value) => {
      received = value;
    });
    assert.strictEqual(emitter.emit('ping', 42), true);
    assert.strictEqual(received, 42);
    assert.strictEqual(emitter.listenerCount('ping'), 1);
  },
};
