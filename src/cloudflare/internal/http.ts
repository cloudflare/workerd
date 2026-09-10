// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export interface FetchHandler {
  // We don't use typeof fetch here because that would use the client-side signature,
  // which is different from the server-side signature.
  fetch: (request: Request, env?: unknown, ctx?: unknown) => Promise<Response>;
}

type PortEntry = {
  refs: number;
  reusePort: boolean;
  handler?: FetchHandler;
};

const EPHEMERAL_PORT_MIN = 49152;
const EPHEMERAL_PORT_MAX = 65535;

// Per-isolate virtual local port table for one protocol. The underlying
// transport does not honor local binding, but explicitly bound ports are
// recorded and conflict-checked so that they are unique within the isolate,
// and an entry may carry the handler that inbound sockets on that port are
// routed to.
export class PortTable {
  #entries = new Map<number, PortEntry>();
  #nextEphemeral = EPHEMERAL_PORT_MIN;

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

  // Records port, or shares it when both the holder and this binder set
  // reusePort. Returns false on conflict.
  tryBind(port: number, reusePort = false): boolean {
    const entry = this.#entries.get(port);
    if (entry === undefined) {
      this.#entries.set(port, { refs: 1, reusePort });
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
      this.#entries.delete(port);
    }
  }

  setHandler(port: number, handler: FetchHandler): void {
    const entry = this.#entries.get(port);
    if (entry !== undefined) entry.handler = handler;
  }

  getHandler(port: number): FetchHandler | undefined {
    return this.#entries.get(port)?.handler;
  }
}

export const tcpPorts = new PortTable();
