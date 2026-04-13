// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

/* eslint-disable @typescript-eslint/no-deprecated,@typescript-eslint/restrict-plus-operands */

import { Agent as HttpAgent } from 'node-internal:internal_http_agent';
import type { AgentOptions } from 'node:http';
import type { RequestOptions } from 'node:https';

export class Agent extends HttpAgent {
  maxCachedSessions: number = 100;

  constructor(options?: AgentOptions) {
    // Match Node.js: the HTTPS Agent constructor sets defaultPort and protocol
    // on the options object before passing them to the base HTTP Agent constructor,
    // so they end up on both this.options and this.defaultPort/this.protocol.
    /* eslint-disable @typescript-eslint/no-unsafe-assignment,
       @typescript-eslint/no-unsafe-member-access,
       @typescript-eslint/no-unsafe-argument,
       @typescript-eslint/no-explicit-any */
    const opts = { ...options } as any;
    opts.defaultPort ??= 443;
    opts.protocol ??= 'https:';
    super(opts);
    /* eslint-enable @typescript-eslint/no-unsafe-assignment,
       @typescript-eslint/no-unsafe-member-access,
       @typescript-eslint/no-unsafe-argument,
       @typescript-eslint/no-explicit-any */
  }

  override getName(options: RequestOptions = {}): string {
    let name = super.getName(options);

    name += ':';
    if (options.ca) name += options.ca;

    name += ':';
    if (options.cert) name += options.cert;

    name += ':';
    if (options.clientCertEngine) name += options.clientCertEngine;

    name += ':';
    if (options.ciphers) name += options.ciphers;

    name += ':';
    // eslint-disable-next-line @typescript-eslint/no-base-to-string
    if (options.key) name += options.key;

    name += ':';
    // eslint-disable-next-line @typescript-eslint/no-base-to-string
    if (options.pfx) name += options.pfx;

    name += ':';
    if (options.rejectUnauthorized !== undefined)
      name += options.rejectUnauthorized;

    name += ':';
    if (options.servername && options.servername !== options.host)
      name += options.servername;

    name += ':';
    if (options.minVersion) name += options.minVersion;

    name += ':';
    if (options.maxVersion) name += options.maxVersion;

    name += ':';
    if (options.secureProtocol) name += options.secureProtocol;

    name += ':';
    if (options.crl) name += options.crl;

    name += ':';
    if (options.honorCipherOrder !== undefined)
      name += options.honorCipherOrder;

    name += ':';
    if (options.ecdhCurve) name += options.ecdhCurve;

    name += ':';
    if (options.dhparam) name += options.dhparam;

    name += ':';
    if (options.secureOptions !== undefined) name += options.secureOptions;

    name += ':';
    if (options.sessionIdContext) name += options.sessionIdContext;

    name += ':';
    if (options.sigalgs) name += JSON.stringify(options.sigalgs);

    name += ':';
    if (options.privateKeyIdentifier) name += options.privateKeyIdentifier;

    name += ':';
    if (options.privateKeyEngine) name += options.privateKeyEngine;

    return name;
  }
}

export const globalAgent = new Agent({
  keepAlive: true,
  scheduling: 'lifo',
  timeout: 5000,
});
