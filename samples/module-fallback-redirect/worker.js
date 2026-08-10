// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { load } from 'dynamic-test-dependency';
import { value as staticValue } from 'static-test-dependency';

export default {
  async fetch() {
    console.log('static import:', staticValue);
    const dynamicModule = await load();
    console.log('dynamic import:', dynamicModule.value);
    return new Response('ok');
  },
};
