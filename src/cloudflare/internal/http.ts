// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import sockets from 'cloudflare-internal:sockets';

export interface FetchHandler {
  // We don't use typeof fetch here because that would use the client-side signature,
  // which is different from the server-side signature.
  fetch: (request: Request, env?: unknown, ctx?: unknown) => Promise<Response>;
}

// The platform socket delivered to a worker's connect() handler.
export interface InboundSocket {
  opened: Promise<{
    remoteAddress?: string | null;
    localAddress?: string | null;
  }>;
}

export interface ConnectHandler {
  // Resolves when the connection is finished; the inbound request lives that long.
  connect: (
    socket: InboundSocket,
    env?: unknown,
    ctx?: unknown
  ) => Promise<void>;
}

export type PortHandler = FetchHandler | ConnectHandler;

type PortEntry = {
  // 0 for a declared port that no one has claimed.
  refs: number;
  reusePort: boolean;
  // A port the platform delivers inbound connections on; the entry outlives its binders.
  declared: boolean;
  // Held strongly: a listening server the user keeps no reference to must stay
  // routable for the scope's lifetime, as it would in Node. Several handlers
  // share a reusePort entry and are selected round-robin, as SO_REUSEPORT
  // balances across listeners.
  handlers: PortHandler[];
};

const EPHEMERAL_PORT_MIN = 49152;
const EPHEMERAL_PORT_MAX = 65535;

// Synthetic addresses from 240.1.0.0/16, a reserved block that is never routed
// (Hyperdrive's synthetic hosts use 240.0.0.0/16). Every namespace gets a host
// address so that a connected or accepted socket never reports the unspecified
// address as its own, and peers the platform does not identify appear behind a
// single gateway address with distinct ports, as they would behind a NAT.
const HOST_PREFIX = '240.1.';
export const GATEWAY_ADDRESS = '240.1.255.254';
let nextHostSuffix = 1;

function allocateHostAddress(): string {
  const suffix = nextHostSuffix;
  // Stay below the gateway at .255.254.
  nextHostSuffix = suffix >= 0xfffd ? 1 : suffix + 1;
  return `${HOST_PREFIX}${suffix >> 8}.${suffix & 0xff}`;
}

// Virtual local port table for one protocol within one scope. The underlying
// transport does not honor local binding, but explicitly bound ports are
// recorded and conflict-checked so that they are unique within the scope, and
// an entry may carry the handlers that inbound sockets on that port are routed
// to.
export class PortTable {
  // The namespace's own address, reported where Linux would report the
  // interface a connection actually uses.
  readonly hostAddress = allocateHostAddress();
  #entries = new Map<number, PortEntry>();
  #nextEphemeral = EPHEMERAL_PORT_MIN;
  // Backstop for socket owners that are never closed: a request's IoContext
  // teardown runs no JS, so a socket it strands would otherwise hold its entry
  // for the scope's lifetime. Deterministic release by the owner remains
  // primary. Listening servers are not registered; their entry is meant to
  // outlive any reference to them.
  // FinalizationRegistry is absent on compat dates before enable_weak_ref.
  #registry: FinalizationRegistry<number> | undefined;

  // Next ephemeral port that is not recorded. This is a label, not a
  // reservation: autobound sockets whose request ends before they are destroyed
  // never run cleanup, so recording them would leak entries. Returns 0 when the
  // range is exhausted.
  ephemeral(): number {
    for (let i = EPHEMERAL_PORT_MIN; i <= EPHEMERAL_PORT_MAX; i++) {
      const candidate = this.#nextEphemeral;
      this.#nextEphemeral =
        candidate === EPHEMERAL_PORT_MAX ? EPHEMERAL_PORT_MIN : candidate + 1;
      if (!this.#entries.has(candidate)) return candidate;
    }
    return 0;
  }

  // Marks port as one the platform delivers inbound connections on.
  declare(port: number): void {
    if (!this.#entries.has(port)) {
      this.#entries.set(port, {
        refs: 0,
        reusePort: false,
        declared: true,
        handlers: [],
      });
    }
  }

  hasDeclared(): boolean {
    for (const entry of this.#entries.values()) {
      if (entry.declared) return true;
    }
    return false;
  }

  isDeclared(port: number): boolean {
    return this.#entries.get(port)?.declared ?? false;
  }

  // The first declared port with no binder, or 0.
  unclaimedDeclared(): number {
    for (const [port, entry] of this.#entries) {
      if (entry.declared && entry.refs === 0) return port;
    }
    return 0;
  }

  // Records port, or shares it when both the holder and this binder set
  // reusePort. An unclaimed declared port is claimable. Returns false on
  // conflict.
  tryBind(port: number, reusePort = false): boolean {
    const entry = this.#entries.get(port);
    if (entry === undefined) {
      this.#entries.set(port, {
        refs: 1,
        reusePort,
        declared: false,
        handlers: [],
      });
    } else if (entry.refs === 0) {
      entry.refs = 1;
      entry.reusePort = reusePort;
    } else if (entry.reusePort && reusePort) {
      entry.refs++;
    } else {
      return false;
    }
    return true;
  }

  release(port: number): void {
    const entry = this.#entries.get(port);
    if (entry !== undefined && --entry.refs === 0) {
      if (entry.declared) {
        entry.handlers = [];
      } else {
        this.#entries.delete(port);
      }
    }
  }

  // Releases port when owner is collected without having released it. An owner
  // must unregister before releasing so the entry is never decremented twice.
  register(owner: object, port: number): void {
    if (this.#registry === undefined) {
      if (typeof FinalizationRegistry !== 'function') return;
      this.#registry = new FinalizationRegistry<number>((p) => {
        this.release(p);
      });
    }
    this.#registry.register(owner, port, owner);
  }

  unregister(owner: object): void {
    this.#registry?.unregister(owner);
  }

  setHandler(port: number, handler: PortHandler): void {
    this.#entries.get(port)?.handlers.push(handler);
  }

  clearHandler(port: number, handler: PortHandler): void {
    const entry = this.#entries.get(port);
    if (entry === undefined) return;
    const i = entry.handlers.indexOf(handler);
    if (i !== -1) entry.handlers.splice(i, 1);
  }

  getHandler(port: number): PortHandler | undefined {
    const entry = this.#entries.get(port);
    if (entry === undefined || entry.handlers.length === 0) return undefined;
    const handler = entry.handlers.shift() as PortHandler;
    entry.handlers.push(handler);
    return handler;
  }
}

// A Durable Object instance has its own table for its lifetime and binds ports
// as a separate host would; everything else in the isolate shares one table,
// seeded with the ports the platform delivers inbound connections on, so that a
// server is reachable from every request.
const isolateTcpPorts = new PortTable();
for (const listener of sockets.getInboundListeners()) {
  if (listener.protocol === 'tcp') isolateTcpPorts.declare(listener.port);
}
const scopedTcpPorts = new WeakMap<object, PortTable>();

// The TCP port table for the current scope. Owners capture the table they bound
// in and release through it: release may run in another request's context, or
// in none at all.
export function tcpPorts(): PortTable {
  const key = sockets.getPortScopeKey();
  if (key === undefined) return isolateTcpPorts;
  let table = scopedTcpPorts.get(key);
  if (table === undefined) {
    table = new PortTable();
    scopedTcpPorts.set(key, table);
  }
  return table;
}

// Inbound routing: a Durable Object's table shadows the isolate table.
export function lookupHandler(port: number): PortHandler | undefined {
  return tcpPorts().getHandler(port) ?? isolateTcpPorts.getHandler(port);
}
