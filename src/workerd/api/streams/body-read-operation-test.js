// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, strictEqual, rejects } from 'node:assert';

function collect() {
  gc({ type: 'major', execution: 'sync' });
  gc({ type: 'major', execution: 'sync' });
}

async function failureOf(promise) {
  try {
    await promise;
  } catch (error) {
    return error;
  }
  throw new Error('Expected a rejection');
}

function body(chunks) {
  return new Response(
    new ReadableStream({
      start(controller) {
        for (const chunk of chunks) controller.enqueue(chunk);
        controller.close();
      },
    }),
    { headers: { 'Content-Type': 'application/x-www-form-urlencoded' } }
  );
}

export const validBodies = {
  async test() {
    const chunks = [
      new Uint8Array(0),
      new Uint8Array([65, 0]),
      new Uint8Array([66]),
    ];
    deepStrictEqual(
      [...new Uint8Array(await body(chunks).arrayBuffer())],
      [65, 0, 66]
    );
    strictEqual(await body(chunks).text(), 'A\0B');
    strictEqual(await body([]).text(), '');
    strictEqual((await body([]).arrayBuffer()).byteLength, 0);
    const bom = new Uint8Array([0xef, 0xbb, 0xbf, 65]);
    strictEqual(await body([bom]).text(), await new Response(bom).text());
    deepStrictEqual(
      [...new Uint8Array(await body([bom]).arrayBuffer())],
      [...bom]
    );
    const form = 'a=hello+world&b=%00%FF';
    const expected = await new Response(form, {
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    }).formData();
    const bytes = new TextEncoder().encode(form);
    deepStrictEqual(
      [...(await body([bytes.subarray(0, 5), bytes.subarray(5)]).formData())],
      [...expected]
    );
  },
};

export const bodyReadUsesOperationLocalBytes = {
  async test() {
    for (const method of ['text', 'arrayBuffer', 'formData']) {
      for (const donorSize of [8, 16, 32]) {
        const original = Promise.prototype.then;
        const donorBody = body([
          new TextEncoder().encode('x=' + 'D'.repeat(donorSize - 2)),
        ]);
        const intercepted = new Error('custom completion handler');
        let donor;
        let notifications = 0;
        Promise.prototype.then = function (resolve, reject) {
          Promise.prototype.then = original;
          return original.call(
            this,
            (notification) => {
              ++notifications;
              donor = notification;
              reject(intercepted);
            },
            reject
          );
        };
        try {
          strictEqual(await failureOf(donorBody.text()), intercepted);
          strictEqual(notifications, 1);
          // Completion is notification-only; buffered parts remain local to each read.
          strictEqual(donor, undefined);
          const text = 'x=' + 'V'.repeat(14);
          const target = body([new TextEncoder().encode(text)]);
          Promise.prototype.then = function (resolve, reject) {
            Promise.prototype.then = original;
            return original.call(this, () => resolve(donor), reject);
          };
          const value = await target[method]();
          strictEqual(
            method === 'text'
              ? value
              : method === 'arrayBuffer'
                ? new TextDecoder().decode(value)
                : 'x=' + value.get('x'),
            text
          );
        } finally {
          Promise.prototype.then = original;
        }
      }
    }
  },
};

export const pendingBodyReadRequiresCompletion = {
  async test() {
    for (const method of ['arrayBuffer', 'text']) {
      let controller;
      const response = new Response(
        new ReadableStream({
          start(value) {
            controller = value;
            controller.enqueue(new Uint8Array(0));
          },
        })
      );
      await 0;
      const original = Promise.prototype.then;
      let inner;
      const pending = response[method]();
      Promise.prototype.then = function (resolve) {
        Promise.prototype.then = original;
        inner = this;
        resolve(undefined);
      };
      try {
        const failure = await failureOf(pending);
        strictEqual(failure instanceof TypeError, true);
        strictEqual(failure.message, 'Invalid stream read result.');
        strictEqual(inner instanceof Promise, true);
        collect();
        controller.enqueue(new Uint8Array([65]));
        controller.close();
        await inner;
      } finally {
        Promise.prototype.then = original;
      }
    }
  },
};

export const pendingReadAcrossGc = {
  async test() {
    for (const method of ['arrayBuffer', 'text']) {
      let controller;
      const response = new Response(
        new ReadableStream({
          start(value) {
            controller = value;
          },
        })
      );
      const pending = response[method]();
      await 0;
      collect();
      controller.enqueue(new Uint8Array([65, 66]));
      await 0;
      collect();
      controller.close();
      const value = await pending;
      strictEqual(
        typeof value === 'string' ? value : new TextDecoder().decode(value),
        'AB'
      );
    }
  },
};

export const pendingReadRejectionAcrossGc = {
  async test() {
    for (const method of ['arrayBuffer', 'text']) {
      let controller;
      const response = new Response(
        new ReadableStream({
          start(value) {
            controller = value;
          },
        })
      );
      const reason = new Error('pending read failure');
      const rejected = rejects(response[method](), (error) => error === reason);
      await 0;
      collect();
      controller.error(reason);
      await rejected;
    }
  },
};

export const callbacksRetainPendingBodyRead = {
  async test() {
    for (const kind of ['Request', 'Response']) {
      for (const method of ['arrayBuffer', 'text']) {
        for (const outcome of ['close', 'error', 'cancel']) {
          let controller;
          let finishCancel;
          let cancelReason;
          let cancelCount = 0;
          const stream = new ReadableStream({
            start(value) {
              controller = value;
              controller.enqueue(new Uint8Array(0));
            },
            cancel(reason) {
              ++cancelCount;
              cancelReason = reason;
              return new Promise((resolve) => {
                finishCancel = resolve;
              });
            },
          });
          const target =
            kind === 'Request'
              ? new Request('https://example.test/', {
                  method: 'POST',
                  body: stream,
                })
              : new Response(stream);
          const original = Object.getOwnPropertyDescriptor(
            Promise.prototype,
            'then'
          );
          const early = new Error('adoption failed');
          let inner;
          await 0;
          const outer = target[method]();
          Object.defineProperty(Promise.prototype, 'then', {
            configurable: true,
            get() {
              Object.defineProperty(Promise.prototype, 'then', original);
              inner = this;
              throw early;
            },
          });
          try {
            strictEqual(await failureOf(outer), early);
            strictEqual(inner instanceof Promise, true);
            let settled = false;
            let failure;
            const done = inner.then(
              () => {
                settled = true;
              },
              (error) => {
                settled = true;
                failure = error;
              }
            );
            collect();
            const reason = new Error('later read failed');
            if (outcome === 'close') {
              controller.enqueue(new Uint8Array([65]));
              controller.close();
            } else if (outcome === 'error') {
              controller.error(reason);
            } else {
              controller.enqueue('not bytes');
              for (let i = 0; i < 10 && finishCancel === undefined; ++i)
                await 0;
              strictEqual(cancelCount, 1);
              strictEqual(cancelReason instanceof TypeError, true);
              strictEqual(settled, false);
              collect();
              finishCancel();
            }
            await done;
            strictEqual(settled, true);
            strictEqual(failure, outcome === 'error' ? reason : cancelReason);
          } finally {
            Object.defineProperty(Promise.prototype, 'then', original);
          }
        }
      }
    }
  },
};
