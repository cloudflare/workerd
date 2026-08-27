// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Property surface of TextEncoderStream and TextDecoderStream.

import { strictEqual } from 'node:assert';

export const encoderEncoding = {
  test() {
    strictEqual(new TextEncoderStream().encoding, 'utf-8');
  },
};

export const decoderOptionsReflection = {
  test() {
    // The label is normalized ('utf-16' names the LE variant) and the
    // options are reflected by the getters.
    const stream = new TextDecoderStream('utf-16', {
      fatal: true,
      ignoreBOM: true,
    });
    strictEqual(stream.encoding, 'utf-16le');
    strictEqual(stream.fatal, true);
    strictEqual(stream.ignoreBOM, true);
  },
};
