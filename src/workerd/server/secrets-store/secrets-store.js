// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Secrets Store: the entrypoint `SecretsStoreSecret`, a secret's store and
// name in its props:
//
//   (name = "KEY", service = (name = "<this service>",
//     entrypoint = "SecretsStoreSecret", props = (json =
//       "{\"store_id\": \"demo\", \"secret_name\": \"api-key\"}")))
//
// `get()` resolves to the secret's value or throws `Secret "<name>" not
// found`, as Miniflare's does. As on Cloudflare, a Worker can only read a
// secret. Nothing is stored: the value is this Worker's binding
// `env:<secret_name>`, which cli/wrangler.rs binds `fromEnvironment`, so a
// secret of one name has one value in every store.
//
// The config that runs this module (`bundled = "/workerd/secrets-store.js"`)
// is an interface: cli/wrangler.rs and hand-written configs write it.
// Anything added to it later must be optional.

import { WorkerEntrypoint } from 'cloudflare:workers';

export class SecretsStoreSecret extends WorkerEntrypoint {
  async get() {
    const name = this.ctx.props.secret_name;
    const value = this.env[`env:${name}`];
    if (typeof value !== 'string') {
      throw new Error(`Secret "${name}" not found`);
    }
    return value;
  }
}
