// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-386:
// URLSearchParams key/value iterators must return owning copies of strings,
// not borrowed pointers into the query vector. A re-entrant mutation via
// Object.prototype setter during JSG_STRUCT slow-path wrapping must not
// cause a use-after-free.

import { strictEqual } from 'node:assert';

// Test that a key iterator returns the correct value even when an
// Object.prototype.done setter mutates the URLSearchParams mid-iteration.
// Pre-patch, this would read freed memory (UAF).
export const keyIteratorReentrantDelete = {
  test() {
    const key0 = 'A'.repeat(64);
    const key1 = 'B'.repeat(64);
    const usp = new URLSearchParams();
    usp.append(key0, 'v0');
    usp.append(key1, 'v1');

    const it = usp.keys();

    let armed = true;
    Object.defineProperty(Object.prototype, 'done', {
      configurable: true,
      set(v) {
        if (armed) {
          armed = false;
          // This delete frees the kj::String buffer backing key0.
          // Pre-patch, the pending kj::StringPtr in the Next struct
          // would become dangling.
          usp.delete(key0);
        }
      },
      get() {
        return undefined;
      },
    });

    try {
      const r = it.next();
      // The value must be the original key0, not garbage from freed memory.
      strictEqual(
        r.value,
        key0,
        'key iterator must return an owning copy of the key, ' +
          'not a dangling pointer'
      );
    } finally {
      delete Object.prototype.done;
    }
  },
};

// Same test for the value iterator.
export const valueIteratorReentrantDelete = {
  test() {
    const key0 = 'X'.repeat(64);
    const val0 = 'Y'.repeat(64);
    const usp = new URLSearchParams();
    usp.append(key0, val0);
    usp.append('Z'.repeat(64), 'w1');

    const it = usp.values();

    let armed = true;
    Object.defineProperty(Object.prototype, 'done', {
      configurable: true,
      set(v) {
        if (armed) {
          armed = false;
          usp.delete(key0);
        }
      },
      get() {
        return undefined;
      },
    });

    try {
      const r = it.next();
      strictEqual(
        r.value,
        val0,
        'value iterator must return an owning copy of the value, ' +
          'not a dangling pointer'
      );
    } finally {
      delete Object.prototype.done;
    }
  },
};

// Same test for the entry iterator.
export const entryIteratorReentrantDelete = {
  test() {
    const key0 = 'X'.repeat(64);
    const val0 = 'Y'.repeat(64);
    const usp = new URLSearchParams();
    usp.append(key0, val0);
    usp.append('Z'.repeat(64), 'w1');

    const it = usp.entries();

    let armed = true;
    Object.defineProperty(Object.prototype, 'done', {
      configurable: true,
      set(v) {
        if (armed) {
          armed = false;
          usp.delete(key0);
        }
      },
      get() {
        return undefined;
      },
    });

    try {
      const r = it.next();
      strictEqual(
        r.value[0],
        key0,
        'entry iterator must return an owning copy of the key, ' +
          'not a dangling pointer'
      );
      strictEqual(
        r.value[1],
        val0,
        'entry iterator must return an owning copy of the value, ' +
          'not a dangling pointer'
      );
    } finally {
      delete Object.prototype.done;
    }
  },
};
