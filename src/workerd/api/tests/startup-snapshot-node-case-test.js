// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A case of startup-snapshot-node-test.js: MODE names the object the top level creates.

import { AsyncLocalStorage, AsyncResource } from 'node:async_hooks';
import diagnosticsChannel from 'node:diagnostics_channel';
import { env } from 'cloudflare:workers';
// Whether a Worker was started from a startup snapshot is asked at request time: at module scope
// the answer would be the zygote's and be baked into the snapshot.
import unsafe from 'workerd:unsafe';

const MODE = env.MODE;

// An instance of a JavaScript subclass is a wrapper around the same native AsyncResource.
class Handler extends AsyncResource {
  constructor() {
    super('handler');
  }

  storeOf(als) {
    return this.runInAsyncScope(() => als.getStore());
  }
}

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
  case 'async-resource':
    // AsyncResource::snapshotClone(). A resource created in the root frame captures no frame.
    state = {
      als: new AsyncLocalStorage(),
      resource: new AsyncResource('startup'),
      handler: new Handler(),
    };
    break;
  case 'async-resource-frame': {
    // A resource created inside a frame holds that frame, which its clone cannot carry, so the
    // zygote rejects the snapshot.
    const als = new AsyncLocalStorage();
    state = {
      als,
      resource: als.run('framed', () => new AsyncResource('startup')),
    };
    break;
  }
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
    case 'async-resource': {
      const { als, resource, handler } = state;
      const receiver = {};
      const bound = resource.bind(function (a, b) {
        return this === receiver && a + b === 3 && als.getStore() === undefined;
      }, receiver);
      return {
        ids: resource.asyncId() === 0 && resource.triggerAsyncId() === 0,
        // The resource belongs to the root frame, so its scope leaves the caller's frame.
        rootFrame:
          als.run(1, () => resource.runInAsyncScope(() => als.getStore())) ===
          undefined,
        callerFrameRestored:
          als.run(1, () => {
            resource.runInAsyncScope(() => {});
            return als.getStore();
          }) === 1,
        receiverAndArguments: resource.runInAsyncScope(
          function (a, b) {
            return this === receiver && a + b === 3;
          },
          receiver,
          1,
          2
        ),
        bind: als.run(1, () => bound(1, 2)),
        boundResource: bound.asyncResource === resource,
        subclass:
          handler instanceof Handler &&
          als.run(1, () => handler.storeOf(als)) === undefined,
      };
    }
    case 'async-resource-frame': {
      const { als, resource } = state;
      return {
        capturedFrame:
          resource.runInAsyncScope(() => als.getStore()) === 'framed',
        outside: als.getStore() === undefined,
      };
    }
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
