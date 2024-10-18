// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright © web-platform-tests contributors. BSD license

import { strictEqual } from 'node:assert';

globalThis.fetch = async (url) => {
  const { default: data } = await import(url);
  return {
    async json() {
      return data;
    },
  };
};

globalThis.promise_test = (callback) => {
  callback();
};

globalThis.assert_equals = (a, b, c) => {
  strictEqual(a, b, c);
};

globalThis.test = (callback, message) => {
  try {
    callback();
  } catch (err) {
    globalThis.errors.push(new AggregateError([err], message));
  }
};

globalThis.errors = [];

export function prepare() {
  globalThis.errors = [];
}

export function validate() {
  if (globalThis.errors.length > 0) {
    for (const err of globalThis.errors) {
      console.error(err);
    }
    throw new Error('Test failed');
  }
}
