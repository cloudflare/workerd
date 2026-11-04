// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without set_tostring_tag, EventSource is untagged.

import { strictEqual } from 'node:assert';
import { streamOf } from 'helpers';

export const legacyToStringTag = {
  test() {
    strictEqual(
      Object.getOwnPropertyDescriptor(
        EventSource.prototype,
        Symbol.toStringTag
      ),
      undefined
    );
    const source = EventSource.from(streamOf([], { close: false }));
    strictEqual(Object.prototype.toString.call(source), '[object Object]');
    source.close();
  },
};
