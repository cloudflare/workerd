// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as harness from 'harness';

export const urlConstructor = {
  async test() {
    harness.prepare();
    await import('url-constructor.any.js');
    harness.validate();
  },
};

export const urlOrigin = {
  async test() {
    harness.prepare();
    await import('url-origin.any.js');
    harness.validate();
  },
};
