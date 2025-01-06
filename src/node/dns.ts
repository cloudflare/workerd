// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import * as errorCodes from 'node-internal:internal_dns_constants';
import * as dns from 'node-internal:internal_dns';
import { callbackify } from 'node-internal:internal_utils';
import * as dnsPromises from 'node-internal:internal_dns_promises';

export * from 'node-internal:internal_dns_constants';

export const promises = dnsPromises;
export const reverse = callbackify(dns.reverse.bind(dns));
export const resolveTxt = callbackify(dns.resolveTxt.bind(dns));
export const resolveCaa = callbackify(dns.resolveCaa.bind(dns));
export const resolveMx = callbackify(dns.resolveMx.bind(dns));
export const resolveCname = callbackify(dns.resolveCname.bind(dns));
export const resolveNs = callbackify(dns.resolveNs.bind(dns));
export const resolvePtr = callbackify(dns.resolvePtr.bind(dns));
export const resolveSrv = callbackify(dns.resolveSrv.bind(dns));
export const resolveSoa = callbackify(dns.resolveSoa.bind(dns));
export const resolveNaptr = callbackify(dns.resolveNaptr.bind(dns));
export const resolve4 = callbackify(dns.resolve4.bind(dns));
export const resolve6 = callbackify(dns.resolve6.bind(dns));
export const getServers = callbackify(dns.getServers.bind(dns));
export const setServers = callbackify(dns.setServers.bind(dns));
export const getDefaultResultOrder = callbackify(
  dns.getDefaultResultOrder.bind(dns)
);
export const setDefaultResultOrder = callbackify(
  dns.setDefaultResultOrder.bind(dns)
);
export const lookup = callbackify(dns.lookup.bind(this));
export const lookupService = callbackify(dns.lookupService.bind(this));
export const resolve = callbackify(dns.resolve.bind(this));
export const resolveAny = callbackify(dns.resolveAny.bind(this));

export class Resolver {
  public cancel(): void {
    // TODO(soon): Implement this.
    throw new Error('Not implemented');
  }

  public setLocalAddress(): void {
    // Does not apply to workerd implementation
    throw new Error('Not implemented');
  }

  public getServers(callback: never): void {
    getServers(callback);
  }

  public resolve(callback: never): void {
    resolve(callback);
  }

  public resolve4(
    input: string,
    options?: { ttl?: boolean },
    callback?: never
  ): void {
    // @ts-expect-error TS2554 TODO(soon): Fix callbackify typescript types
    resolve4(input, options, callback);
  }

  public resolve6(
    input: string,
    options?: { ttl?: boolean },
    callback?: never
  ): void {
    // @ts-expect-error TS2554 TODO(soon): Fix callbackify typescript types
    resolve6(input, options, callback);
  }

  public resolveAny(_input: string, callback: never): void {
    resolveAny(callback);
  }

  public resolveCaa(name: string, callback: never): void {
    resolveCaa(name, callback);
  }

  public resolveCname(name: string, callback: never): void {
    resolveCname(name, callback);
  }

  public resolveMx(name: string, callback: never): void {
    resolveMx(name, callback);
  }

  public resolveNaptr(name: string, callback: never): void {
    resolveNaptr(name, callback);
  }

  public resolveNs(name: string, callback: never): void {
    resolveNs(name, callback);
  }

  public resolvePtr(name: string, callback: never): void {
    resolvePtr(name, callback);
  }

  public resolveSoa(name: string, callback: never): void {
    resolveSoa(name, callback);
  }

  public resolveSrv(name: string, callback: never): void {
    resolveSrv(name, callback);
  }

  public resolveTxt(name: string, callback: never): void {
    resolveTxt(name, callback);
  }

  public reverse(name: string, callback: never): void {
    reverse(name, callback);
  }

  public setServers(callback: never): void {
    setServers(callback);
  }
}

export default {
  getServers,
  lookup,
  lookupService,
  resolve,
  resolve4,
  resolve6,
  resolveAny,
  resolveCname,
  resolveCaa,
  resolveMx,
  resolveNaptr,
  resolveNs,
  resolvePtr,
  resolveSoa,
  resolveSrv,
  resolveTxt,
  reverse,
  setDefaultResultOrder,
  getDefaultResultOrder,
  setServers,
  Resolver,
  promises,
  ...errorCodes,
};
