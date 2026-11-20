// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What sendBeacon() does at the top level of a module, outside any request.
// send-beacon.js asserts what is recorded here.

import { caught } from 'helpers';

const log = [];
const logs = (name, result) => ({
  toString() {
    log.push(name);
    return result;
  },
});

export const topLevel = {
  // No request to schedule the beacon on.
  plain: navigator.sendBeacon('http://sink/top-level'),
  withBody: navigator.sendBeacon('http://sink/top-level', 'body'),
  invalidUrl: navigator.sendBeacon('not a url'),
  // The arguments are converted all the same.
  converted: navigator.sendBeacon(
    logs('url', 'http://sink/top-level'),
    logs('body', 'body')
  ),
  log,
  // And every error is thrown as within a request.
  missingArgument: caught(() => navigator.sendBeacon()),
  symbolUrl: caught(() => navigator.sendBeacon(Symbol('url'))),
  symbolBody: caught(() => navigator.sendBeacon('http://sink/', Symbol('b'))),
  receiver: caught(() => navigator.sendBeacon.call({}, 'http://sink/')),
};
