// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { RpcTarget, WorkerEntrypoint } from 'cloudflare:workers';

function responseFor(props, input, init, label) {
  const request = new Request(input, init);
  if (new URL(request.url).origin !== 'https://api.example') {
    throw new TypeError('Destination must be an allowed HTTPS origin');
  }
  return new Response(
    JSON.stringify({
      ...props,
      label: label ?? null,
      legacySelector: null,
      method: request.method,
      url: request.url,
    })
  );
}

class Identity extends RpcTarget {
  #props;
  #label;
  constructor(props, label) {
    super();
    this.#props = Object.freeze({ ...props });
    this.#label = label;
  }
  get id() {
    return Promise.resolve(
      `${this.#props.workerId}:${this.#props.namespaceId}:${this.#label ?? 'workload'}`
    );
  }
  get signatureAgent() {
    return this.id.then((id) => `https://${id}.example`);
  }
  get directoryURL() {
    return this.id.then((id) => `https://${id}.example/directory`);
  }
  fetch(input, init) {
    return responseFor(this.#props, input, init, this.#label);
  }
}

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
  getIdentity(label, ...extra) {
    if (
      extra.length ||
      (label !== undefined &&
        (typeof label !== 'string' || !/^[A-Za-z0-9_-]{1,128}$/.test(label)))
    ) {
      throw new TypeError('getIdentity accepts only an optional label');
    }
    return new Identity(this.ctx.props, label);
  }
  fetch() {
    throw new TypeError('Use getIdentity().fetch() for signed requests');
  }
}
