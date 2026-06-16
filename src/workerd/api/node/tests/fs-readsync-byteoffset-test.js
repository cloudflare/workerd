// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';
import { openSync, closeSync, readSync, writeFileSync } from 'node:fs';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-13
export const readSyncByteOffsetRegressionTest = {
  test() {
    writeFileSync('/tmp/byteoffset-test.txt', 'ATTACK');
    const ab = new ArrayBuffer(13);
    const full = new Uint8Array(ab);
    full.set([83, 69, 67, 82, 69, 84, 124, 80, 85, 66, 76, 73, 67]);
    const view = new Uint8Array(ab, 7, 6);
    const fd = openSync('/tmp/byteoffset-test.txt', 'r');
    try {
      readSync(fd, view, { offset: 0, length: 6, position: 0 });
    } finally {
      closeSync(fd);
    }
    strictEqual(new TextDecoder().decode(view), 'ATTACK');
    strictEqual(full[0], 83);
    strictEqual(full[6], 124);
  },
};

export const readSyncNumericOffsetTest = {
  test() {
    writeFileSync('/tmp/numoffset-test.txt', 'HELLO');
    const ab = new ArrayBuffer(16);
    new Uint8Array(ab).fill(65);
    const view = new Uint8Array(ab, 8, 5);
    const fd = openSync('/tmp/numoffset-test.txt', 'r');
    try {
      readSync(fd, view, 0, 5, 0);
    } finally {
      closeSync(fd);
    }
    strictEqual(new TextDecoder().decode(view), 'HELLO');
    strictEqual(new Uint8Array(ab, 0, 8)[0], 65);
  },
};

// Nonzero numeric offset argument + undefined length: length should default to
// (buffer.byteLength - offset) of the *view*, and the read should land within
// the view starting at the given offset, not corrupting bytes outside.
export const readSyncNonzeroOffsetUndefinedLengthTest = {
  test() {
    writeFileSync('/tmp/nz-offset-undef-len-test.txt', 'XYZ');
    const ab = new ArrayBuffer(16);
    new Uint8Array(ab).fill(65); // 'A' everywhere
    const view = new Uint8Array(ab, 4, 8); // view spans ab[4..12)
    const fd = openSync('/tmp/nz-offset-undef-len-test.txt', 'r');
    try {
      // offset=2 within the view, length omitted (undefined), position=0.
      readSync(fd, view, 2, undefined, 0);
    } finally {
      closeSync(fd);
    }
    // Data should land at view[2..5) == ab[6..9).
    const full = new Uint8Array(ab);
    // Bytes outside the view must remain 'A'.
    for (let i = 0; i < 4; i++) strictEqual(full[i], 65);
    for (let i = 12; i < 16; i++) strictEqual(full[i], 65);
    // Bytes inside the view before the offset must remain 'A'.
    strictEqual(full[4], 65);
    strictEqual(full[5], 65);
    // Data 'XYZ' should appear at ab[6..9).
    strictEqual(full[6], 0x58); // 'X'
    strictEqual(full[7], 0x59); // 'Y'
    strictEqual(full[8], 0x5a); // 'Z'
  },
};

// Same scenario via the options-object form: nonzero offset, length omitted.
export const readSyncNonzeroOffsetUndefinedLengthOptionsTest = {
  test() {
    writeFileSync('/tmp/nz-offset-undef-len-opts-test.txt', 'XYZ');
    const ab = new ArrayBuffer(16);
    new Uint8Array(ab).fill(65);
    const view = new Uint8Array(ab, 4, 8);
    const fd = openSync('/tmp/nz-offset-undef-len-opts-test.txt', 'r');
    try {
      readSync(fd, view, { offset: 2, position: 0 });
    } finally {
      closeSync(fd);
    }
    const full = new Uint8Array(ab);
    for (let i = 0; i < 4; i++) strictEqual(full[i], 65);
    for (let i = 12; i < 16; i++) strictEqual(full[i], 65);
    strictEqual(full[4], 65);
    strictEqual(full[5], 65);
    strictEqual(full[6], 0x58);
    strictEqual(full[7], 0x59);
    strictEqual(full[8], 0x5a);
  },
};
