// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, rejects } from 'node:assert';

// Adversarial regression tests for AUTOVULN-CLOUDFLARE-WORKERD-289.
//
// Each test exercises a SubtleCrypto method with a re-entrant path: a property
// getter on a JSG_STRUCT algorithm parameter fires during argument unwrapping
// and either resizes or detaches an ArrayBuffer that has already been (or is
// about to be) captured by the runtime. Before the fix, this could leave a
// stale {pointer, length} pair pointing into decommitted pages → SIGSEGV.
//
// The tests verify:
//   1. The getter actually fired (re-entrancy occurred).
//   2. The call did not crash (reached the assertion after the call).
//   3. The call threw a clean JS error (not an internal error / segfault).

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function makeResizable(size) {
  const buf = new ArrayBuffer(size, { maxByteLength: size });
  new Uint8Array(buf).fill(0xaa);
  return buf;
}

async function importAesGcmKey(...usages) {
  return crypto.subtle.importKey(
    'raw',
    new Uint8Array(16),
    { name: 'AES-GCM' },
    false,
    usages
  );
}

async function importHmacKey() {
  return crypto.subtle.importKey(
    'raw',
    new Uint8Array(32),
    { name: 'HMAC', hash: 'SHA-256' },
    false,
    ['sign', 'verify']
  );
}

// ---------------------------------------------------------------------------
// encrypt — struct-before-buffer path
// ---------------------------------------------------------------------------

export const encryptResize = {
  async test() {
    const key = await importAesGcmKey('encrypt');
    const buf = makeResizable(256 * 1024);

    let getterFired = false;
    const alg = {
      name: 'AES-GCM',
      iv: new Uint8Array(12),
      get tagLength() {
        buf.resize(1);
        getterFired = true;
        return 128;
      },
    };

    await crypto.subtle.encrypt(alg, key, buf);

    ok(getterFired, 'getter did not fire');
  },
};

export const encryptResizeToZero = {
  async test() {
    const key = await importAesGcmKey('encrypt');
    const buf = makeResizable(256 * 1024);

    let getterFired = false;
    const alg = {
      name: 'AES-GCM',
      iv: new Uint8Array(12),
      get tagLength() {
        buf.resize(0);
        getterFired = true;
        return 128;
      },
    };

    await crypto.subtle.encrypt(alg, key, buf);

    ok(getterFired, 'getter did not fire');
  },
};

export const encryptDetach = {
  async test() {
    const key = await importAesGcmKey('encrypt');
    const buf = makeResizable(256 * 1024);

    let getterFired = false;
    const alg = {
      name: 'AES-GCM',
      iv: new Uint8Array(12),
      get tagLength() {
        // Transfer detaches the original buffer.
        structuredClone(buf, { transfer: [buf] });
        getterFired = true;
        return 128;
      },
    };

    await crypto.subtle.encrypt(alg, key, buf);

    ok(getterFired, 'getter did not fire');
  },
};

// ---------------------------------------------------------------------------
// decrypt — struct-before-buffer path
// ---------------------------------------------------------------------------

export const decryptResize = {
  async test() {
    const key = await importAesGcmKey('decrypt');
    // Ciphertext must be at least tagLength/8 = 16 bytes for AES-GCM.
    const buf = makeResizable(256 * 1024);

    let getterFired = false;
    const alg = {
      name: 'AES-GCM',
      iv: new Uint8Array(12),
      get tagLength() {
        buf.resize(1);
        getterFired = true;
        return 128;
      },
    };

    await rejects(crypto.subtle.decrypt(alg, key, buf), {
      message: /Ciphertext length of 8 bits must be/,
    });

    ok(getterFired, 'getter did not fire');
  },
};

// ---------------------------------------------------------------------------
// sign — struct-before-buffer path
// ---------------------------------------------------------------------------

export const signResize = {
  async test() {
    const key = await importHmacKey();
    const buf = makeResizable(256 * 1024);

    let getterFired = false;
    const alg = {
      name: 'HMAC',
      get hash() {
        buf.resize(1);
        getterFired = true;
        return undefined; // hash was set at import time
      },
    };

    await crypto.subtle.sign(alg, key, buf);

    ok(getterFired, 'getter did not fire');
  },
};

// ---------------------------------------------------------------------------
// verify — struct-before-buffer path, two buffer args
// ---------------------------------------------------------------------------

export const verifyResize = {
  async test() {
    const key = await importHmacKey();

    // First, produce a valid signature so verify gets past initial checks.
    const realData = new Uint8Array(32);
    const sig = await crypto.subtle.sign('HMAC', key, realData);

    // Now replay with a resizable data buffer that gets shrunk.
    const dataBuf = makeResizable(256 * 1024);

    let getterFired = false;
    const alg = {
      name: 'HMAC',
      get hash() {
        dataBuf.resize(1);
        getterFired = true;
        return undefined;
      },
    };

    await crypto.subtle.verify(alg, key, sig, dataBuf);

    ok(getterFired, 'getter did not fire');
  },
};

// ---------------------------------------------------------------------------
// digest — simplest path, fewest params
// ---------------------------------------------------------------------------

export const digestResize = {
  async test() {
    const buf = makeResizable(256 * 1024);

    let getterFired = false;
    const alg = {
      get name() {
        buf.resize(1);
        getterFired = true;
        return 'SHA-256';
      },
    };

    await crypto.subtle.digest(alg, buf);

    ok(getterFired, 'getter did not fire');
  },
};

export const digestDetach = {
  async test() {
    const buf = makeResizable(256 * 1024);

    let getterFired = false;
    const alg = {
      get name() {
        structuredClone(buf, { transfer: [buf] });
        getterFired = true;
        return 'SHA-256';
      },
    };

    await crypto.subtle.digest(alg, buf);

    ok(getterFired, 'getter did not fire');
  },
};

// ---------------------------------------------------------------------------
// importKey — TOCTOU: buffer-before-struct
// ---------------------------------------------------------------------------

export const importKeyDetach = {
  async test() {
    const buf = makeResizable(16);

    let getterFired = false;
    const alg = {
      name: 'AES-GCM',
      get length() {
        structuredClone(buf, { transfer: [buf] });
        getterFired = true;
        return 128;
      },
    };

    await rejects(
      crypto.subtle.importKey('raw', buf, alg, false, ['encrypt']),
      {
        message: /Imported AES key length must be/,
      }
    );

    ok(getterFired, 'getter did not fire');
  },
};

// ---------------------------------------------------------------------------
// unwrapKey — original autovuln-289 path: buffer-before-struct
// ---------------------------------------------------------------------------

export const unwrapKeyDetach = {
  async test() {
    const key = await importAesGcmKey('unwrapKey');
    const buf = makeResizable(256 * 1024);
    const iv = new Uint8Array(12);

    let getterFired = false;
    const unwrapAlg = {
      name: 'AES-GCM',
      iv,
      get tagLength() {
        structuredClone(buf, { transfer: [buf] });
        getterFired = true;
        return 128;
      },
    };

    await rejects(
      crypto.subtle.unwrapKey(
        'raw',
        buf,
        key,
        unwrapAlg,
        { name: 'AES-GCM' },
        false,
        ['encrypt']
      ),
      {
        message: /Ciphertext length of 0 bits must be/,
      }
    );

    ok(getterFired, 'getter did not fire');
  },
};

// ===========================================================================
// Original TOCTOU test cases (unwrapKey/importKey with resize)
// ===========================================================================

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

    await rejects(
      crypto.subtle.unwrapKey(
        'raw',
        buf,
        key,
        unwrapAlg,
        { name: 'AES-GCM' },
        false,
        ['encrypt']
      ),
      {
        message: /Ciphertext length of 8 bits must be/,
      }
    );

    ok(getterFired, 'getter did not fire');
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

    await rejects(
      crypto.subtle.importKey('raw', buf, alg, false, ['encrypt']),
      {
        message: /Imported AES key length must be/,
      }
    );

    ok(getterFired, 'getter did not fire');
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

    await rejects(
      crypto.subtle.unwrapKey(
        'raw',
        view,
        key,
        unwrapAlg,
        { name: 'AES-GCM' },
        false,
        ['encrypt']
      ),
      {
        message: /Ciphertext length of 8 bits must be/,
      }
    );

    ok(getterFired, 'getter did not fire');
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

    await rejects(
      crypto.subtle.importKey('raw', view, alg, false, ['encrypt']),
      {
        message: /Imported AES key length must be/,
      }
    );

    ok(getterFired, 'getter did not fire');
  },
};
