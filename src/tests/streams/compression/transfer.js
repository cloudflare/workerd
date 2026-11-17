// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone over the compression streams rejects DataCloneError
// in both implementations, as for any platform object without a
// serialization.

import { throws } from 'node:assert';

function notSerializable(type) {
  return {
    name: 'DataCloneError',
    message: `Could not serialize object of type "${type}". This type does not support serialization.`,
  };
}

export const structuredCloneCompressionStreams = {
  test() {
    for (const stream of [
      new CompressionStream('gzip'),
      new DecompressionStream('gzip'),
    ]) {
      throws(
        () => structuredClone(stream),
        notSerializable(stream.constructor.name)
      );
    }
  },
};
