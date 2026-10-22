// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { connect } from 'cloudflare:sockets';
import { deepStrictEqual, rejects, strictEqual } from 'node:assert';

// A refused connection rejects `opened` and `closed` with a plain Error that carries no cause.
// node:net reports the same failure as ECONNREFUSED by reading the cause through an internal
// channel (getConnectFailureCode), so a change to this public shape must keep that in step.
export const connectRefused = {
  async test() {
    const socket = connect('127.0.0.1:1');
    for (const promise of [socket.opened, socket.closed]) {
      await rejects(promise, (err) => {
        strictEqual(err.constructor, Error);
        strictEqual(
          err.message,
          'proxy request failed, cannot connect to the specified address'
        );
        deepStrictEqual(Object.keys(err), []);
        strictEqual('code' in err, false);
        strictEqual('errno' in err, false);
        return true;
      });
    }
  },
};
