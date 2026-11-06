// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// util.inspect() formats ports and channels as JSG resource types: their
// prototype accessors' values, except function-valued ones.

import { strictEqual } from 'node:assert';
import { inspect } from 'node:util';

export const inspectPort = {
  test() {
    const { port1 } = new MessageChannel();
    strictEqual(inspect(port1), 'MessagePort { onmessage: null }');
    port1.onmessage = function handler() {};
    strictEqual(inspect(port1), 'MessagePort {}');
    port1.close();
  },
};

export const inspectChannel = {
  test() {
    const channel = new MessageChannel();
    strictEqual(
      inspect(channel),
      'MessageChannel {\n' +
        '  port2: MessagePort { onmessage: null },\n' +
        '  port1: MessagePort { onmessage: null }\n' +
        '}'
    );
  },
};

export const inspectPrototype = {
  test() {
    strictEqual(
      inspect(MessagePort.prototype).replace(
        'EventTarget [MessagePort]',
        'EventTarget'
      ),
      'EventTarget {\n' +
        '  postMessage: [Function: postMessage],\n' +
        '  close: [Function: close],\n' +
        '  start: [Function: start],\n' +
        '  onmessage: [Getter/Setter]\n' +
        '}'
    );
  },
};
