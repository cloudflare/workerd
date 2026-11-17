// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone over the queuing strategies rejects DataCloneError in
// both implementations, as for any platform object without a
// serialization.

import { throws } from 'node:assert';

function notSerializable(type) {
  return {
    name: 'DataCloneError',
    message: `Could not serialize object of type "${type}". This type does not support serialization.`,
  };
}

export const structuredCloneStrategies = {
  test() {
    for (const strategy of [
      new ByteLengthQueuingStrategy({ highWaterMark: 1 }),
      new CountQueuingStrategy({ highWaterMark: 1 }),
    ]) {
      throws(
        () => structuredClone(strategy),
        notSerializable(strategy.constructor.name)
      );
    }
  },
};
