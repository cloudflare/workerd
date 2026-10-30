// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared helpers for the compression stream benchmarks.

// Cases check that they did the work their names claim, so a stream that
// ends early fails the case instead of reporting a fast time.
export function expectEqual(actual, expected, what) {
  if (actual !== expected) {
    throw new Error(`${what}: expected ${expected}, got ${actual}`);
  }
}

export function sizeLabel(bytes) {
  return bytes >= 1024 ? `${bytes / 1024} KiB` : `${bytes} B`;
}

// How chunked() splits `total` bytes into chunks of `size`.
export function chunksLabel(total, size) {
  return size >= total ? 'in one chunk' : `in ${sizeLabel(size)} chunks`;
}

export const FORMATS = ['gzip', 'deflate', 'deflate-raw'];

// 64 KiB of text that compresses about as well as markup does: words drawn
// from a small vocabulary by a fixed pseudo-random sequence.
export const PAYLOAD = (() => {
  const words = [
    'stream',
    'worker',
    'request',
    'response',
    'body',
    'chunk',
    'header',
    'cache',
    'fetch',
    'event',
    '<div>',
    '</div>',
    'class="item"',
    '{"id":',
    '},',
  ];
  const encoder = new TextEncoder();
  const bytes = new Uint8Array(65536);
  let filled = 0;
  let seed = 1;
  while (filled < bytes.length) {
    seed = (seed * 1103515245 + 12345) % 2147483648;
    const word = encoder.encode(words[seed % words.length] + ' ');
    const n = Math.min(word.length, bytes.length - filled);
    bytes.set(word.subarray(0, n), filled);
    filled += n;
  }
  return bytes;
})();

// Throws unless `bytes`, a size PAYLOAD compressed to in a first run, is a
// compression's: PAYLOAD compresses to about a seventh of its size. A case
// whose expected size comes from a first run checks it with this, since a
// stream that passed the payload through or ended early would match itself
// on every run.
export function expectCompressedSize(bytes) {
  if (bytes <= 0 || bytes >= PAYLOAD.byteLength / 2) {
    throw new Error(`The payload compressed to ${bytes} bytes.`);
  }
}

// `bytes` split into chunks of `size` bytes. Writes don't modify their
// chunks, so the cases write the same views every call.
export function chunked(bytes, size) {
  const chunks = [];
  for (let offset = 0; offset < bytes.length; offset += size) {
    chunks.push(bytes.subarray(offset, offset + size));
  }
  return chunks;
}

// Writes `chunks` to `stream` and closes it, while the readable side is read
// to the end; returns the number of bytes read.
export async function pump(stream, chunks) {
  const writer = stream.writable.getWriter();
  const reader = stream.readable.getReader();
  const drained = (async () => {
    let bytes = 0;
    for (;;) {
      const { value, done } = await reader.read();
      if (done) return bytes;
      bytes += value.byteLength;
    }
  })();
  const writes = chunks.map((chunk) => writer.write(chunk));
  const [bytes] = await Promise.all([drained, ...writes, writer.close()]);
  return bytes;
}

// `bytes` compressed in `format`, in one piece.
export async function compress(bytes, format) {
  const stream = new CompressionStream(format);
  const writer = stream.writable.getWriter();
  writer.write(bytes);
  writer.close();
  return new Uint8Array(await new Response(stream.readable).arrayBuffer());
}
