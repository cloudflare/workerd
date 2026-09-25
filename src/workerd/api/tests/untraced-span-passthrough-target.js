// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { DurableObject } from 'cloudflare:workers';

export default {
  async fetch(request) {
    return new Response('target');
  },
};

export class TargetObject extends DurableObject {
  async fetch(request) {
    return new Response('target-object');
  }
}
