// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export const unwrapKeyResizableBuffer = {
  async test() {
    const key = await crypto.subtle.importKey(
      'raw',
      new Uint8Array(16),
      { name: 'AES-GCM' },
      false,
      ['unwrapKey']
    );

    const buf = new ArrayBuffer(256 * 1024, {
      maxByteLength: 256 * 1024,
    });
    new Uint8Array(buf).fill(0xaa);
    const iv = new Uint8Array(12);

    let getterFired = false;
    const unwrapAlg = {
      name: 'AES-GCM',
      iv,
      get tagLength() {
        buf.resize(1);
        getterFired = true;
        return 128;
      },
    };

    let threw = false;
    try {
      await crypto.subtle.unwrapKey(
        'raw',
        buf,
        key,
        unwrapAlg,
        { name: 'AES-GCM' },
        false,
        ['encrypt']
      );
    } catch {
      threw = true;
    }

    if (!getterFired) {
      throw new Error('getter did not fire');
    }
    if (!threw) {
      throw new Error('unwrapKey should have thrown');
    }
  },
};

export const importKeyResizableBuffer = {
  async test() {
    const buf = new ArrayBuffer(256 * 1024, {
      maxByteLength: 256 * 1024,
    });
    new Uint8Array(buf).fill(0xbb);

    let getterFired = false;
    const alg = {
      name: 'AES-GCM',
      get length() {
        buf.resize(1);
        getterFired = true;
        return 128;
      },
    };

    let threw = false;
    try {
      await crypto.subtle.importKey('raw', buf, alg, false, ['encrypt']);
    } catch {
      threw = true;
    }

    if (!getterFired) {
      throw new Error('getter did not fire');
    }
    if (!threw) {
      throw new Error('importKey should have thrown');
    }
  },
};

// ArrayBufferView variants: exercises the asBytes(v8::ArrayBufferView) overload.
// The view is a Uint8Array over a resizable ArrayBuffer; the getter shrinks the
// underlying buffer while the view's {byteOffset, byteLength} still refer to
// the original extent.

export const unwrapKeyResizableBufferView = {
  async test() {
    const key = await crypto.subtle.importKey(
      'raw',
      new Uint8Array(16),
      { name: 'AES-GCM' },
      false,
      ['unwrapKey']
    );

    const buf = new ArrayBuffer(256 * 1024, {
      maxByteLength: 256 * 1024,
    });
    const view = new Uint8Array(buf);
    view.fill(0xcc);
    const iv = new Uint8Array(12);

    let getterFired = false;
    const unwrapAlg = {
      name: 'AES-GCM',
      iv,
      get tagLength() {
        buf.resize(1);
        getterFired = true;
        return 128;
      },
    };

    let threw = false;
    try {
      await crypto.subtle.unwrapKey(
        'raw',
        view,
        key,
        unwrapAlg,
        { name: 'AES-GCM' },
        false,
        ['encrypt']
      );
    } catch {
      threw = true;
    }

    if (!getterFired) {
      throw new Error('getter did not fire');
    }
    if (!threw) {
      throw new Error('unwrapKey should have thrown');
    }
  },
};

export const importKeyResizableBufferView = {
  async test() {
    const buf = new ArrayBuffer(256 * 1024, {
      maxByteLength: 256 * 1024,
    });
    const view = new Uint8Array(buf);
    view.fill(0xdd);

    let getterFired = false;
    const alg = {
      name: 'AES-GCM',
      get length() {
        buf.resize(1);
        getterFired = true;
        return 128;
      },
    };

    let threw = false;
    try {
      await crypto.subtle.importKey('raw', view, alg, false, ['encrypt']);
    } catch {
      threw = true;
    }

    if (!getterFired) {
      throw new Error('getter did not fire');
    }
    if (!threw) {
      throw new Error('importKey should have thrown');
    }
  },
};
