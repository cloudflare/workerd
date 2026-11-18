// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Secrets Store secret whose value the embedder supplies in `value`: a
// string, or null when the secret is not set. Workers can only read it.
class SecretsStoreSecretImpl {
  readonly #name: string;
  readonly #value: unknown;

  constructor(name: string, value: unknown) {
    this.#name = name;
    this.#value = value;
  }

  async get(): Promise<string> {
    if (typeof this.#value !== 'string') {
      throw new Error(`Secret "${this.#name}" not found`);
    }
    return this.#value;
  }
}

export function makeBinding(env: {
  name: string;
  value: unknown;
}): SecretsStoreSecretImpl {
  return new SecretsStoreSecretImpl(env.name, env.value);
}

export default makeBinding;
