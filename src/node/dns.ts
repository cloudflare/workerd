// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import * as errorCodes from 'node-internal:internal_dns_constants';
import * as dns from 'node-internal:internal_dns';
import { callbackify } from 'node-internal:internal_utils';

export * from 'node-internal:internal_dns_constants';

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

  ...errorCodes,
};
