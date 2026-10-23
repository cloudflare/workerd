// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Reader members called with a reader of another class as `this` fail
// WebIDL's brand check with a TypeError 'Illegal invocation' and leave that
// reader as it was (parity). TypeScript rejects from the members that
// return a promise, as WebIDL has them; C++ throws from every member, the
// shape WPT's idlharness pins for a plain `this`. Under TS the draining
// reader is checked too, as the owner of the members and as `this`.

/* global ReadableStreamDrainingReader */

import { deepStrictEqual, rejects, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const READERS = {
  default: ReadableStreamDefaultReader,
  byob: ReadableStreamBYOBReader,
};
const MEMBERS = {
  default: ['closed', 'cancel', 'read', 'releaseLock'],
  byob: ['closed', 'cancel', 'read', 'readAtLeast', 'releaseLock'],
};
if (usingTsImpl) {
  READERS.draining = ReadableStreamDrainingReader;
  MEMBERS.draining = [
    'closed',
    'expectedLength',
    'cancel',
    'read',
    'releaseLock',
  ];
}

function readerOf(kind) {
  let controller;
  const rs = new ReadableStream({
    type: 'bytes',
    start(c) {
      controller = c;
    },
  });
  let reader;
  if (kind === 'byob') reader = rs.getReader({ mode: 'byob' });
  else if (kind === 'draining') reader = new ReadableStreamDrainingReader(rs);
  else reader = rs.getReader();
  return { rs, controller, reader, kind };
}

function invoke(owner, name, self) {
  const proto = READERS[owner].prototype;
  const { get, value } = Object.getOwnPropertyDescriptor(proto, name);
  if (get !== undefined) return get.call(self);
  if (name === 'cancel') return value.call(self, 'reason');
  if (name === 'readAtLeast') return value.call(self, 1, new Uint8Array(4));
  if (name === 'read' && owner === 'byob') {
    return value.call(self, new Uint8Array(4));
  }
  return value.call(self);
}

// A draining read's bytes are its first chunk.
function bytesOf(result) {
  return [...(result.chunks?.[0] ?? result.value)];
}

// The reader's next read takes the next chunk: the reader was neither
// released nor cancelled, and no read waits ahead of its own.
async function assertUntouched(s) {
  strictEqual(s.rs.locked, true);
  s.controller.enqueue(new Uint8Array([1, 2]));
  const read =
    s.kind === 'byob' ? s.reader.read(new Uint8Array(4)) : s.reader.read();
  deepStrictEqual(bytesOf(await read), [1, 2]);
}

// The members WebIDL makes reject, rather than throw, when the brand check
// fails.
const PROMISE_MEMBERS = new Set(['closed', 'cancel', 'read', 'readAtLeast']);

// C++ adds a pointer to its documentation after the colon.
const ILLEGAL_INVOCATION = {
  name: 'TypeError',
  message: /^Illegal invocation/,
};

// Without its brand check, a read acts on the other reader and stays
// pending; it resolves 'pending' here, so the test names the member rather
// than hanging.
function settledOrPending(promise) {
  return Promise.race([promise, scheduler.wait(250).then(() => 'pending')]);
}

async function checkMember(owner, name, kind) {
  const s = readerOf(kind);
  const call = () => invoke(owner, name, s.reader);
  if (usingTsImpl && PROMISE_MEMBERS.has(name)) {
    await rejects(settledOrPending(call()), ILLEGAL_INVOCATION);
  } else {
    throws(call, ILLEGAL_INVOCATION);
  }
  await assertUntouched(s);
}

export const readerMembersOnOtherReaderClasses = {
  async test() {
    for (const owner of Object.keys(READERS)) {
      for (const name of MEMBERS[owner]) {
        for (const kind of Object.keys(READERS)) {
          if (kind === owner) continue;
          try {
            await checkMember(owner, name, kind);
          } catch (e) {
            e.message = `${owner}.${name} on a ${kind} reader: ${e.message}`;
            throw e;
          }
        }
      }
    }
  },
};
