// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Ports come only from `new MessageChannel()`. Calling either constructor
// without `new` throws the language's TypeError, not JSG's.

import { ok, throws } from 'node:assert';

export const messagePortIsNotConstructible = {
  test() {
    throws(() => new MessagePort(), {
      name: 'TypeError',
      message: 'Illegal constructor',
    });
    class Sub extends MessagePort {}
    throws(() => new Sub(), {
      name: 'TypeError',
      message: 'Illegal constructor',
    });
  },
};

export const constructorsRequireNew = {
  test() {
    throws(() => MessagePort(), {
      name: 'TypeError',
      message: "Class constructor MessagePort cannot be invoked without 'new'",
    });
    throws(() => MessageChannel(), {
      name: 'TypeError',
      message:
        "Class constructor MessageChannel cannot be invoked without 'new'",
    });
  },
};

export const messageChannelIsSubclassable = {
  test() {
    class Sub extends MessageChannel {}
    const channel = new Sub();
    ok(channel instanceof MessageChannel);
    ok(channel.port1 instanceof MessagePort);
    channel.port1.close();
  },
};
