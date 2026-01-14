// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import type nodejsDns from 'node:dns'
import * as dns from 'node-internal:internal_dns'
import * as errorCodes from 'node-internal:internal_dns_constants'
import * as dnsPromises from 'node-internal:internal_dns_promises'
import { callbackify } from 'node-internal:internal_utils'

export * from 'node-internal:internal_dns_constants'

export const promises = dnsPromises
export const reverse = callbackify(dns.reverse.bind(dns))
export const resolveTxt = callbackify(dns.resolveTxt.bind(dns))
export const resolveCaa = callbackify(dns.resolveCaa.bind(dns))
export const resolveMx = callbackify(dns.resolveMx.bind(dns))
export const resolveCname = callbackify(dns.resolveCname.bind(dns))
export const resolveNs = callbackify(dns.resolveNs.bind(dns))
export const resolvePtr = callbackify(dns.resolvePtr.bind(dns))
export const resolveSrv = callbackify(dns.resolveSrv.bind(dns))
export const resolveSoa = callbackify(dns.resolveSoa.bind(dns))
export const resolveNaptr = callbackify(dns.resolveNaptr.bind(dns))
export const resolve4 = callbackify(dns.resolve4.bind(dns))
export const resolve6 = callbackify(dns.resolve6.bind(dns))
export const getServers = dns.getServers.bind(dns)
export const setServers = dns.setServers.bind(dns)
export const getDefaultResultOrder = dns.getDefaultResultOrder.bind(dns)
export const setDefaultResultOrder = dns.setDefaultResultOrder.bind(dns)
export const lookup = dns.lookup.bind(dns)
export const lookupService = callbackify(dns.lookupService.bind(this))
export const resolve = callbackify(dns.resolve.bind(this))
export const resolveAny = callbackify(dns.resolveAny.bind(this))

export class Resolver implements nodejsDns.Resolver {
  cancel(): void {
    // TODO(soon): Implement this.
    throw new Error('Not implemented')
  }

  setLocalAddress(): void {
    // Does not apply to workerd implementation
    throw new Error('Not implemented')
  }

  getServers(...args: Parameters<typeof getServers>): string[] {
    return getServers(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolve(...args: Parameters<typeof resolve>): void {
    resolve(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolve4(...args: Parameters<typeof resolve4>): void {
    resolve4(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolve6(...args: Parameters<typeof resolve6>): void {
    resolve6(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolveAny(...args: Parameters<typeof resolveAny>): void {
    resolveAny(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolveCaa(...args: Parameters<typeof resolveCaa>): void {
    resolveCaa(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolveCname(...args: Parameters<typeof resolveCname>): void {
    resolveCname(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolveMx(...args: Parameters<typeof resolveMx>): void {
    resolveMx(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolveNaptr(...args: Parameters<typeof resolveNaptr>): void {
    resolveNaptr(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolveNs(...args: Parameters<typeof resolveNs>): void {
    resolveNs(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolvePtr(...args: Parameters<typeof resolvePtr>): void {
    resolvePtr(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolveSoa(...args: Parameters<typeof resolveSoa>): void {
    resolveSoa(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolveSrv(...args: Parameters<typeof resolveSrv>): void {
    resolveSrv(...args)
  }

  // @ts-expect-error TS2416 Type mismatch.
  resolveTxt(...args: Parameters<typeof resolveTxt>): void {
    resolveTxt(...args)
  }

  reverse(...args: Parameters<typeof reverse>): void {
    reverse(...args)
  }

  setServers(...args: Parameters<typeof setServers>): void {
    setServers(...args)
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
}
