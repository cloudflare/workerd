// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Encoder and decoder streams composed with pipeThrough.

import { strictEqual } from 'node:assert';

export const encoderDecoderPipeline = {
  async test() {
    // string -> bytes -> string -> bytes across three chained transforms.
    const encIn = new TextEncoderStream();
    const dec = new TextDecoderStream();
    const encOut = new TextEncoderStream();

    const end = encIn.readable.pipeThrough(dec).pipeThrough(encOut);

    const writer = encIn.writable.getWriter();
    await writer.write('hello');
    await writer.close();

    const result = await end.getReader().read();
    strictEqual(new TextDecoder().decode(result.value), 'hello');
  },
};
