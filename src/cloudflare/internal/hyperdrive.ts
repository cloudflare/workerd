// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import wrappedBinding from 'cloudflare-internal:wrapped-binding';
import { type Socket } from 'cloudflare-internal:sockets';

// The inner fetcher is an ExtendedFetcher: a service binding to the Hyperdrive RPC worker that
// also exposes a random host and fixed port which `cloudflare:sockets` connect() routes through it.
interface HyperdriveFetcher {
  readonly host: string;
  readonly port: number;
  connect(address: string): Socket;
}

interface HyperdriveEnv {
  fetcher: HyperdriveFetcher;
}

type HyperdriveScheme = 'postgresql' | 'mysql';

const MYSQL_PORT = 3306;

// 16 random bytes as 32 lowercase hex characters, which need no escaping in a connection string.
function randomHex(): string {
  return Array.from(crypto.getRandomValues(new Uint8Array(16)), (b) =>
    b.toString(16).padStart(2, '0')
  ).join('');
}

export class Hyperdrive extends wrappedBinding.WrappedBinding {
  readonly #fetcher: HyperdriveFetcher;
  readonly #scheme: HyperdriveScheme;
  #database: string | undefined;
  #user: string | undefined;
  #password: string | undefined;

  declare readonly database: string;
  declare readonly user: string;
  declare readonly password: string;
  declare readonly scheme: HyperdriveScheme;
  declare readonly connectionString: string;
  declare readonly host: string;
  declare readonly port: number;
  declare readonly connect: () => Socket;

  constructor(env: HyperdriveEnv) {
    super(env.fetcher);
    this.#fetcher = env.fetcher;
    this.#scheme = this.#fetcher.port === MYSQL_PORT ? 'mysql' : 'postgresql';

    // Own, read-only properties, matching the shape of the native Hyperdrive binding.
    Object.defineProperties(this, {
      database: { enumerable: true, get: (): string => this.#getDatabase() },
      user: { enumerable: true, get: (): string => this.#getUser() },
      password: { enumerable: true, get: (): string => this.#getPassword() },
      scheme: { enumerable: true, get: (): HyperdriveScheme => this.#scheme },
      connectionString: {
        enumerable: true,
        get: (): string => this.#getConnectionString(),
      },
      host: { enumerable: true, get: (): string => this.#fetcher.host },
      port: { enumerable: true, get: (): number => this.#fetcher.port },
      connect: {
        value: (): Socket =>
          this.#fetcher.connect(`${this.#fetcher.host}:${this.#fetcher.port}`),
      },
    });
  }

  // The worker never sees the real database credentials; the Hyperdrive RPC worker holds them and
  // the binding exposes independent random placeholders instead. Each is generated once per binding,
  // on first access: bindings are constructed at global scope, where generating random values is
  // disallowed.
  #getDatabase(): string {
    return (this.#database ??= randomHex());
  }

  #getUser(): string {
    return (this.#user ??= randomHex());
  }

  #getPassword(): string {
    return (this.#password ??= randomHex());
  }

  #getConnectionString(): string {
    const sslOption =
      this.#scheme === 'postgresql' ? 'sslmode=disable' : 'ssl-mode=disabled';
    return `${this.#scheme}://${this.#getUser()}:${this.#getPassword()}@${this.#fetcher.host}:${this.#fetcher.port}/${this.#getDatabase()}?${sslOption}`;
  }
}

export default function makeBinding(env: HyperdriveEnv): Hyperdrive {
  return new Hyperdrive(env);
}
