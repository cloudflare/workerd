// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Ports come only from `new MessageChannel()`.
// Divergence #3 (see AGENTS.md): calling either constructor without `new`
// throws JSG's TypeError for the C++ classes and the language's for the
// TypeScript classes.

import { ok, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

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
      message: usingTsImpl
        ? "Class constructor MessagePort cannot be invoked without 'new'"
        : 'Illegal constructor',
    });
    throws(() => MessageChannel(), {
      name: 'TypeError',
      message: usingTsImpl
        ? "Class constructor MessageChannel cannot be invoked without 'new'"
        : "Failed to construct 'MessageChannel': Please use the 'new' " +
          'operator, this object constructor cannot be called as a function.',
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
