// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Interplay with garbage collection (the configs set --expose-gc). Reader
// and writer handles must keep a collected stream wrapper's underlying
// machinery — including the decoder state the C++ implementation holds via
// a traced ref — alive and operable.

import { strictEqual } from 'node:assert';

export const abortEncoderWriterAfterGc = {
  async test() {
    const writer = (() => new TextEncoderStream().writable.getWriter())();
    gc();
    await writer.abort();
  },
};

export const decodeAfterStreamWrapperGc = {
  async test() {
    const { writer, reader } = (() => {
      const tds = new TextDecoderStream();
      return {
        writer: tds.writable.getWriter(),
        reader: tds.readable.getReader(),
      };
    })();
    gc();
    const readPromise = reader.read();
    await writer.write(new TextEncoder().encode('gc'));
    strictEqual((await readPromise).value, 'gc');
    await writer.close();
    strictEqual((await reader.read()).done, true);
  },
};
