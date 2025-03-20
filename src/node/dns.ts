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
export const getDefaultResultOrder = dns.getDefaultResultOrder.bind(dns);
export const setDefaultResultOrder = dns.setDefaultResultOrder.bind(dns);
export const lookup = dns.lookup.bind(dns);
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

  public getServers(...args: Parameters<typeof getServers>): void {
    getServers(...args);
  }

  public resolve(...args: Parameters<typeof resolve>): void {
    resolve(...args);
  }

  public resolve4(...args: Parameters<typeof resolve4>): void {
    resolve4(...args);
  }

  public resolve6(...args: Parameters<typeof resolve6>): void {
    resolve6(...args);
  }

  public resolveAny(...args: Parameters<typeof resolveAny>): void {
    resolveAny(...args);
  }

  public resolveCaa(...args: Parameters<typeof resolveCaa>): void {
    resolveCaa(...args);
  }

  public resolveCname(...args: Parameters<typeof resolveCname>): void {
    resolveCname(...args);
  }

  public resolveMx(...args: Parameters<typeof resolveMx>): void {
    resolveMx(...args);
  }

  public resolveNaptr(...args: Parameters<typeof resolveNaptr>): void {
    resolveNaptr(...args);
  }

  public resolveNs(...args: Parameters<typeof resolveNs>): void {
    resolveNs(...args);
  }

  public resolvePtr(...args: Parameters<typeof resolvePtr>): void {
    resolvePtr(...args);
  }

  public resolveSoa(...args: Parameters<typeof resolveSoa>): void {
    resolveSoa(...args);
  }

  public resolveSrv(...args: Parameters<typeof resolveSrv>): void {
    resolveSrv(...args);
  }

  public resolveTxt(...args: Parameters<typeof resolveTxt>): void {
    resolveTxt(...args);
  }

  public reverse(...args: Parameters<typeof reverse>): void {
    reverse(...args);
  }

  public setServers(...args: Parameters<typeof setServers>): void {
    setServers(...args);
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
