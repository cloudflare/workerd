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
// found`, as Miniflare's does. `["SecretsStoreSecret::admin_api"]()` answers
// Miniflare's local admin API: `create`, `update`, `duplicate`, `delete`,
// `list` and `get`, with Miniflare's ids and errors, when this Worker has
// `ADMIN`; it reaches every secret in the store. Without `ADMIN` a Worker can
// only `get()`, as on Cloudflare. wrangler's local `secrets-store secret`
// commands write Miniflare's own state, not this store.
//
// The config that runs this module (`bundled = "/workerd/secrets-store.js"`)
// is an interface: cli/wrangler.rs and hand-written configs write it.
// Anything added to it later must be optional. It is a Worker with the
// bindings:
//
//   store:<store_id>     A KV namespace (`/workerd/kv.js`) for each store:
//                        its secrets, keyed by name, each with the metadata
//                        `{ uuid }`.
//   env:<secret_name>    Optional. When not null, the secret's value in every
//                        store, ahead of the stored one; cli/wrangler.rs binds
//                        it `fromEnvironment`, so it is never stored.
//   ADMIN                Optional. JSON `true` turns on the admin API, for
//                        tests and local tools; cli/wrangler.rs never sets it.

import { WorkerEntrypoint } from 'cloudflare:workers';

const ADMIN_API = 'SecretsStoreSecret::admin_api';

export class SecretsStoreSecret extends WorkerEntrypoint {
  get #store() {
    return this.env[`store:${this.ctx.props.store_id}`];
  }

  async get() {
    const name = this.ctx.props.secret_name;
    const value =
      this.env[`env:${name}`] ?? (await this.#store.get(name, 'text'));
    if (value === null) throw new Error(`Secret "${name}" not found`);
    return value;
  }

  [ADMIN_API]() {
    if (this.env.ADMIN !== true) {
      throw new Error('The Secrets Store admin API is off');
    }
    const store = this.#store;
    const name = this.ctx.props.secret_name;
    // The name of the secret `id`, from the first page of the store.
    const find = async (id) => {
      const { keys } = await store.list();
      const secret = keys.find((k) => k.metadata?.uuid === id);
      if (!secret) throw new Error('Secret not found');
      return secret.name;
    };
    return {
      async create(value) {
        const id = crypto.randomUUID().replaceAll('-', '');
        await store.put(name, value, { metadata: { uuid: id } });
        return id;
      },
      async update(value, id) {
        await store.put(await find(id), value, { metadata: { uuid: id } });
        return id;
      },
      async duplicate(id, newName) {
        const value = await store.get(await find(id), 'text');
        if (!value) throw new Error('Secret not found');
        const newId = crypto.randomUUID();
        await store.put(newName, value, { metadata: { uuid: newId } });
        return newId;
      },
      async delete(id) {
        await store.delete(await find(id));
      },
      async list() {
        return (await store.list()).keys;
      },
      get: find,
    };
  }
}
