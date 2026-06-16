// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-330:
// SIGSEGV in SqlStorage::Cursor when a resizable ArrayBuffer bound as a
// SQLITE_STATIC blob is shrunk before the cursor is read.

import * as assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

export class DurableObjectExample extends DurableObject {
  constructor(state, env) {
    super(state, env);
    this.state = state;
  }

  async testResizableArrayBufferBlobBinding() {
    const sql = this.state.storage.sql;

    // Allocate a resizable ArrayBuffer and fill it with a known pattern.
    const rab = new ArrayBuffer(64, { maxByteLength: 256 });
    const view = new Uint8Array(rab);
    for (let i = 0; i < view.length; i++) {
      view[i] = i & 0xff;
    }

    // Bind the resizable ArrayBuffer as a blob parameter.
    const cursor = sql.exec('SELECT ? AS x', rab);

    // Shrink the buffer to zero -- V8 decommits the previously committed pages.
    rab.resize(0);

    // Reading the cursor must not crash (SIGSEGV). After the fix, asBytes()
    // copies the data eagerly when the source ArrayBuffer is resizable, so the
    // kj::Array<const byte> stored in Cursor::State::bindings owns stable heap
    // memory regardless of later resize() calls.
    const rows = cursor.toArray();
    assert.strictEqual(rows.length, 1);

    // The blob should contain the original 64 bytes (copied before resize).
    const blob = new Uint8Array(rows[0].x);
    assert.strictEqual(blob.length, 64);
    for (let i = 0; i < blob.length; i++) {
      assert.strictEqual(
        blob[i],
        i & 0xff,
        `byte at index ${i} should be ${i & 0xff} but got ${blob[i]}`
      );
    }
  }

  async testResizableArrayBufferViewBlobBinding() {
    const sql = this.state.storage.sql;

    // Use a Uint8Array view over a resizable ArrayBuffer.
    const rab = new ArrayBuffer(128, { maxByteLength: 512 });
    const fullView = new Uint8Array(rab);
    for (let i = 0; i < fullView.length; i++) {
      fullView[i] = (i * 3) & 0xff;
    }

    // Create a view over a sub-range.
    const subView = new Uint8Array(rab, 16, 32);

    const cursor = sql.exec('SELECT ? AS x', subView);

    // Shrink the underlying buffer.
    rab.resize(0);

    // Must not crash.
    const rows = cursor.toArray();
    assert.strictEqual(rows.length, 1);

    const blob = new Uint8Array(rows[0].x);
    assert.strictEqual(blob.length, 32);
    for (let i = 0; i < blob.length; i++) {
      const expected = ((i + 16) * 3) & 0xff;
      assert.strictEqual(
        blob[i],
        expected,
        `byte at index ${i} should be ${expected} but got ${blob[i]}`
      );
    }
  }

  async testNonResizableArrayBufferStillWorks() {
    const sql = this.state.storage.sql;

    // Sanity check: non-resizable ArrayBuffer blob binding still works.
    const ab = new ArrayBuffer(8);
    const view = new Uint8Array(ab);
    for (let i = 0; i < view.length; i++) {
      view[i] = 0xaa;
    }

    const rows = [...sql.exec('SELECT ? AS x', ab)];
    assert.strictEqual(rows.length, 1);
    const blob = new Uint8Array(rows[0].x);
    assert.strictEqual(blob.length, 8);
    for (let i = 0; i < blob.length; i++) {
      assert.strictEqual(blob[i], 0xaa);
    }
  }
}

export let testResizableArrayBufferBlobBinding = {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName('rab-blob-test');
    let stub = env.ns.get(id);
    await stub.testResizableArrayBufferBlobBinding();
  },
};

export let testResizableArrayBufferViewBlobBinding = {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName('rab-view-blob-test');
    let stub = env.ns.get(id);
    await stub.testResizableArrayBufferViewBlobBinding();
  },
};

export let testNonResizableArrayBufferStillWorks = {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName('non-rab-blob-test');
    let stub = env.ns.get(id);
    await stub.testNonResizableArrayBufferStillWorks();
  },
};
