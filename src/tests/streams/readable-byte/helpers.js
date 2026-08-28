// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared helpers for the readable-byte suite.

// Reads a byte stream to completion with a default reader, returning
// all bytes concatenated.
export async function drainBytes(readable) {
  const reader = readable.getReader();
  const parts = [];
  let total = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    parts.push(value);
    total += value.byteLength;
  }
  const out = new Uint8Array(total);
  let off = 0;
  for (const p of parts) {
    out.set(p, off);
    off += p.byteLength;
  }
  return out;
}

const FULFILLED = Symbol('fulfilled');
export async function rejectionOf(p) {
  try {
    await p;
    return FULFILLED;
  } catch (e) {
    return e;
  }
}
