// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import * as errorCodes from 'node-internal:internal_dns_constants';
import {
  getServers,
  lookup,
  lookupService,
  resolve,
  resolve4,
  resolve6,
  resolveAny,
  resolveNaptr,
  resolveNs,
  resolvePtr,
  resolveSoa,
  resolveSrv,
  setDefaultResultOrder,
  getDefaultResultOrder,
  setServers,
} from 'node-internal:internal_dns';
import * as dns from 'node-internal:internal_dns';
import { callbackify } from 'node-internal:internal_utils';

export * from 'node-internal:internal_dns_constants';
export {
  getServers,
  lookup,
  lookupService,
  resolve,
  resolve4,
  resolve6,
  resolveAny,
  resolveNaptr,
  resolveNs,
  resolvePtr,
  resolveSoa,
  resolveSrv,
  setDefaultResultOrder,
  getDefaultResultOrder,
  setServers,
} from 'node-internal:internal_dns';

export const reverse = callbackify(dns.reverse.bind(dns));
export const resolveTxt = callbackify(dns.resolveTxt.bind(dns));
export const resolveCaa = callbackify(dns.resolveCaa.bind(dns));
export const resolveMx = callbackify(dns.resolveMx.bind(dns));
export const resolveCname = callbackify(dns.resolveCname.bind(dns));

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
