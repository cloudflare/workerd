// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { Agent as HttpAgent } from 'node-internal:internal_http_agent';

export class Agent extends HttpAgent {
  public override defaultPort = 443;
  public override protocol: string = 'https:';
}

export const globalAgent = new Agent({
  keepAlive: true,
  scheduling: 'lifo',
  timeout: 5000,
});
