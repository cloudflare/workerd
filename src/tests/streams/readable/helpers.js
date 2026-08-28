// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared helpers for the readable suite.

// Reads a stream to completion, returning the chunks as an array.
export async function drainToArray(readable) {
  const reader = readable.getReader();
  const items = [];
  for (;;) {
    const { value, done } = await reader.read();
    if (done) return items;
    items.push(value);
  }
}

// Captures the rejection reason of a promise (asserting it rejects is
// the caller's job via the returned sentinel).
const FULFILLED = Symbol('fulfilled');
export async function rejectionOf(p) {
  try {
    await p;
    return FULFILLED;
  } catch (e) {
    return e;
  }
}
