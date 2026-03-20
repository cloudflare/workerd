// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import type dns from 'node:dns/promises';
import type {
  LookupAddress,
  LookupAllOptions,
  LookupOneOptions,
  LookupOptions,
} from 'node:dns';
import * as errorCodes from 'node-internal:internal_dns_constants';
import {
  reverse,
  resolveTxt,
  resolveCaa,
  resolveMx,
  resolveCname,
  resolveNs,
  resolvePtr,
  resolveSrv,
  resolveSoa,
  resolveNaptr,
  resolve4,
  resolve6,
  getServers,
  setServers,
  getDefaultResultOrder,
  setDefaultResultOrder,
  lookup as internalLookup,
  lookupService,
  resolve,
  resolveAny,
} from 'node-internal:internal_dns';
import type {
  CAA,
  MX,
  NAPTR,
  SOA,
  SRV,
  TTLResponse,
} from 'node-internal:internal_dns_client';

export * from 'node-internal:internal_dns_constants';
export {
  reverse,
  resolveTxt,
  resolveCaa,
  resolveMx,
  resolveCname,
  resolveNs,
  resolvePtr,
  resolveSrv,
  resolveSoa,
  resolveNaptr,
  resolve4,
  resolve6,
  getServers,
  setServers,
  getDefaultResultOrder,
  setDefaultResultOrder,
  lookupService,
  resolve,
  resolveAny,
} from 'node-internal:internal_dns';

export class Resolver implements dns.Resolver {
  cancel(): void {
    // TODO(soon): Implement this.
    throw new Error('Not implemented');
  }

  setLocalAddress(): void {
    // Does not apply to workerd implementation
    throw new Error('Not implemented');
  }

  getServers(): string[] {
    return getServers();
  }

  // @ts-expect-error TS2769 No matching overload.
  resolve(name: string, rrtype: string): ReturnType<typeof resolve> {
    return resolve(name, rrtype);
  }

  // @ts-expect-error TS2769 No matching overload.
  resolve4(
    input: string,
    options?: { ttl?: boolean }
  ): Promise<(string | TTLResponse)[]> {
    return resolve4(input, options);
  }

  // @ts-expect-error TS2769 No matching overload.
  resolve6(
    input: string,
    options?: { ttl?: boolean }
  ): Promise<(string | TTLResponse)[]> {
    return resolve6(input, options);
  }

  // @ts-expect-error TS2769 No matching overload.
  resolveAny(): Promise<void> {
    return resolveAny();
  }

  resolveCaa(name: string): Promise<CAA[]> {
    return resolveCaa(name);
  }

  resolveCname(name: string): Promise<string[]> {
    return resolveCname(name);
  }

  resolveMx(name: string): Promise<MX[]> {
    return resolveMx(name);
  }

  resolveNaptr(name: string): Promise<NAPTR[]> {
    return resolveNaptr(name);
  }

  resolveNs(name: string): Promise<string[]> {
    return resolveNs(name);
  }

  resolvePtr(name: string): Promise<string[]> {
    return resolvePtr(name);
  }

  resolveSoa(name: string): Promise<SOA> {
    return resolveSoa(name);
  }

  resolveSrv(name: string): Promise<SRV[]> {
    return resolveSrv(name);
  }

  resolveTxt(name: string): Promise<string[][]> {
    return resolveTxt(name);
  }

  reverse(name: string): Promise<string[]> {
    return reverse(name);
  }

  setServers(): void {
    setServers();
  }
}

export function lookup(
  hostname: string,
  options?: LookupOptions | LookupOneOptions | LookupAllOptions
): Promise<LookupAddress | LookupAddress[]> {
  const { promise, resolve, reject } = Promise.withResolvers<
    LookupAddress | LookupAddress[]
  >();
  internalLookup(hostname, options, (error, address, family) => {
    if (error) {
      reject(error);
    } else if (Array.isArray(address)) {
      resolve(address);
    } else {
      resolve({
        address,
        family,
      } as LookupAddress);
    }
  });
  return promise;
}

export default {
  reverse,
  resolveTxt,
  resolveCaa,
  resolveMx,
  resolveCname,
  resolveNs,
  resolvePtr,
  resolveSrv,
  resolveSoa,
  resolveNaptr,
  resolve4,
  resolve6,
  getServers,
  setServers,
  getDefaultResultOrder,
  setDefaultResultOrder,
  lookup,
  lookupService,
  resolve,
  resolveAny,
  Resolver,
  ...errorCodes,
};
