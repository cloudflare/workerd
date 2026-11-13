// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A case of startup-snapshot-node-test.js: MODE names the object the top level creates.

import { AsyncLocalStorage } from 'node:async_hooks';
import diagnosticsChannel from 'node:diagnostics_channel';
import { env } from 'cloudflare:workers';
import unsafe from 'workerd:unsafe';

const MODE = env.MODE;

let state = null;

switch (MODE) {
  case 'als':
    // AsyncLocalStorage::snapshotClone(). Importing node:async_hooks alone reaches the
    // stateless AsyncHooksModule.
    state = {
      als: new AsyncLocalStorage(),
      named: new AsyncLocalStorage({ name: 'request' }),
    };
    break;
  case 'als-default':
    // A default value is a JavaScript value, which the clone of an AsyncLocalStorage cannot
    // hold, so the zygote rejects the snapshot.
    state = new AsyncLocalStorage({ defaultValue: 'default' });
    break;
  case 'diagnostics-channel': {
    // node:diagnostics_channel reaches the stateless DiagnosticsChannelModule; its channels and
    // their subscribers are JavaScript.
    const messages = [];
    const channel = diagnosticsChannel.channel('startup-snapshot');
    channel.subscribe((message) => messages.push(message));
    state = { channel, messages };
    break;
  }
}

function checks() {
  switch (MODE) {
    case 'als': {
      const { als, named } = state;
      return {
        run: als.run(1, () => als.getStore()) === 1,
        outside: als.getStore() === undefined,
        nested: als.run(1, () => als.run(2, () => als.getStore())) === 2,
        name: named.name === 'request',
        separateKeys: als.run(1, () => named.getStore()) === undefined,
      };
    }
    case 'als-default':
      return { defaultValue: state.getStore() === 'default' };
    case 'diagnostics-channel': {
      const { channel, messages } = state;
      channel.publish('hello');
      return {
        hasSubscribers: channel.hasSubscribers,
        delivered: messages.length === 1 && messages[0] === 'hello',
      };
    }
  }
  return {};
}

export default {
  async fetch() {
    return Response.json({
      mode: MODE,
      snapshotUsed: unsafe.startedFromStartupSnapshot(),
      checks: checks(),
    });
  },
};
