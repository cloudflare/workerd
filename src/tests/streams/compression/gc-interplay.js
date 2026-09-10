// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Interplay with garbage collection (the configs set --expose-gc). Reader
// and writer handles must keep a collected stream wrapper's underlying
// machinery — including the codec handle — alive and operable.

import { strictEqual } from 'node:assert';

export const abortCompressionWriterAfterGc = {
  async test() {
    const writer = (() => new CompressionStream('gzip').writable.getWriter())();
    gc();
    await writer.abort();
  },
};

export const decompressAfterStreamWrapperGc = {
  async test() {
    const { writer, reader } = (() => {
      const ds = new DecompressionStream('deflate');
      return {
        writer: ds.writable.getWriter(),
        reader: ds.readable.getReader(),
      };
    })();
    gc();
    const readPromise = reader.read();
    await writer.write(new Uint8Array([120, 156, 75, 4, 0, 0, 98, 0, 98])); // deflate('a')
    await writer.close();
    strictEqual(new TextDecoder().decode((await readPromise).value), 'a');
  },
};
