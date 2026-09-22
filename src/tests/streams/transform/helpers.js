// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared consumption helpers for the transform suite.

// Concatenates string chunks from a readable.
export async function consume(readable) {
  let data = '';
  for await (const chunk of readable) {
    data += chunk;
  }
  return data;
}

// Decodes and concatenates byte chunks from a readable.
export async function consumeBytes(readable) {
  const dec = new TextDecoder();
  let data = '';
  for await (const chunk of readable) {
    data += dec.decode(chunk, { stream: true });
  }
  data += dec.decode();
  return data;
}
