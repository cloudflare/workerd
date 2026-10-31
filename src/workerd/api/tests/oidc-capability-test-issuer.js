// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { WorkerEntrypoint } from 'cloudflare:workers';
export class Issuer extends WorkerEntrypoint {
  getToken(...args) {
    if (args.length) throw new TypeError('getToken takes no arguments');
    return { ...this.ctx.props, jti: crypto.randomUUID() };
  }
  getAuthorizationGrant({ authorizationServer }) {
    return {
      ...this.ctx.props,
      audience: authorizationServer,
      jti: crypto.randomUUID(),
    };
  }
  checkSignedFetch() {}
  fetch() {
    return new Response(JSON.stringify(this.ctx.props));
  }
}
