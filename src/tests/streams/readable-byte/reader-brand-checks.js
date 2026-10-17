// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Reader members called with a reader of another class as `this`, which
// WebIDL's brand check rejects with TypeError. C++ throws TypeError from
// every member and leaves that reader as it was. It throws where WebIDL has
// the members that return a promise reject, the shape WPT's idlharness pins
// for a plain `this`.
//
// DEFECT (ledger #44): TypeScript's members reach a reader's state through
// one lookup that accepts every reader class, so most act on the other
// reader: releaseLock() releases it, cancel() cancels its stream, closed is
// its closed promise, a default or draining read() reads from its stream,
// and the draining reader's expectedLength answers for it. The BYOB
// reader's read() and readAtLeast() reject with TypeError and leave it as
// it was. Under TS the draining reader is checked too, as the owner of the
// members and as `this`.

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

// What TypeScript does with a reader of another class (the DEFECT).
async function assertActsOnOtherReader(owner, name, s) {
  switch (name) {
    case 'closed':
      strictEqual(invoke(owner, name, s.reader), s.reader.closed);
      await assertUntouched(s);
      break;
    case 'expectedLength':
      strictEqual(invoke(owner, name, s.reader), undefined);
      await assertUntouched(s);
      break;
    case 'cancel':
      strictEqual(await invoke(owner, name, s.reader), undefined);
      strictEqual(await s.reader.closed, undefined);
      break;
    case 'read': {
      const read = invoke(owner, name, s.reader);
      s.controller.enqueue(new Uint8Array([1, 2]));
      deepStrictEqual(bytesOf(await read), [1, 2]);
      break;
    }
    case 'releaseLock':
      strictEqual(invoke(owner, name, s.reader), undefined);
      strictEqual(s.rs.locked, false);
      break;
  }
}

async function checkMember(owner, name, kind) {
  const s = readerOf(kind);
  if (!usingTsImpl) {
    throws(() => invoke(owner, name, s.reader), TypeError);
    await assertUntouched(s);
  } else if (owner === 'byob' && (name === 'read' || name === 'readAtLeast')) {
    await rejects(invoke(owner, name, s.reader), TypeError);
    await assertUntouched(s);
  } else {
    await assertActsOnOtherReader(owner, name, s);
  }
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
