// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

/* eslint-disable @typescript-eslint/no-deprecated */
import {
  sendDnsRequest,
  validateAnswer,
  normalizeMx,
  normalizeCname,
  normalizeCaa,
  normalizePtr,
  normalizeNaptr,
  normalizeNs,
  normalizeSoa,
  normalizeTxt,
  normalizeSrv,
  type MX,
  type CAA,
  type NAPTR,
  type SOA,
  type SRV,
  type TTLResponse,
} from 'node-internal:internal_dns_client';
import {
  DnsError,
  ERR_INVALID_ARG_TYPE,
  ERR_OPTION_NOT_IMPLEMENTED,
  ERR_INVALID_ARG_VALUE,
} from 'node-internal:internal_errors';
import {
  validateString,
  validateFunction,
  validateOneOf,
  validateNumber,
  validateBoolean,
} from 'node-internal:validators';
import * as errorCodes from 'node-internal:internal_dns_constants';
import { isIP } from 'node-internal:internal_net';
import type dns from 'node:dns';

type DnsOrder = 'verbatim' | 'ipv4first' | 'ipv6first';

export const validFamilies = [0, 4, 6];
export const validDnsOrders: DnsOrder[] = [
  'verbatim',
  'ipv4first',
  'ipv6first',
];

let defaultDnsOrder: DnsOrder = 'verbatim';

export function getServers(): ReturnType<(typeof dns)['getServers']> {
  return ['1.1.1.1', '2606:4700:4700::1111', '1.0.0.1', '2606:4700:4700::1001'];
}

export type LookupCallback = (
  err: Error | null,
  address?: string | { address: string; family: number }[],
  family?: number
) => void;

export function lookup(
  hostname: string,
  options?: dns.LookupOptions | LookupCallback,
  callback?: LookupCallback
): void {
  let family: 0 | 4 | 6 = 0;
  let all = false;
  let dnsOrder: DnsOrder = getDefaultResultOrder();

  // Parse arguments
  if (hostname) {
    validateString(hostname, 'hostname');
  }

  if (typeof options === 'function') {
    callback = options;
    family = 0;
  } else if (typeof options === 'number') {
    validateFunction(callback, 'callback');

    validateOneOf(options, 'family', validFamilies);
    family = options;
  } else if (options !== undefined && typeof options !== 'object') {
    validateFunction(arguments.length === 2 ? options : callback, 'callback');
    throw new ERR_INVALID_ARG_TYPE('options', ['integer', 'object'], options);
  } else {
    validateFunction(callback, 'callback');

    if (options?.hints != null) {
      validateNumber(options.hints, 'options.hints');
      throw new ERR_OPTION_NOT_IMPLEMENTED('options.hints');
    }
    if (options?.family != null) {
      switch (options.family) {
        case 'IPv4':
          family = 4;
          break;
        case 'IPv6':
          family = 6;
          break;
        default:
          validateOneOf(options.family, 'options.family', validFamilies);
          family = options.family as 0 | 4 | 6;
          break;
      }
    }
    if (options?.all != null) {
      validateBoolean(options.all, 'options.all');
      all = options.all;
    }
    if (options?.verbatim != null) {
      validateBoolean(options.verbatim, 'options.verbatim');
      dnsOrder = options.verbatim ? 'verbatim' : 'ipv4first';
    }
    if (options?.order != null) {
      validateOneOf(options.order, 'options.order', validDnsOrders);
      dnsOrder = options.order;
    }
  }

  if (!hostname) {
    if (all) {
      process.nextTick(callback, null, []);
    } else {
      process.nextTick(callback, null, null, family === 6 ? 6 : 4);
    }
    return;
  }

  const matchedFamily = isIP(hostname);
  if (matchedFamily) {
    if (all) {
      process.nextTick(callback, null, [
        { address: hostname, family: matchedFamily },
      ]);
    } else {
      process.nextTick(callback, null, hostname, matchedFamily);
    }
    return;
  }

  // If all is true and family is 0, we need to query both A and AAAA records
  if (all && family === 0) {
    Promise.all([
      sendDnsRequest(hostname, 'A').catch(() => ({ Answer: [] })),
      sendDnsRequest(hostname, 'AAAA').catch(() => ({ Answer: [] })),
    ])
      .then(([ipv4Response, ipv6Response]): void => {
        const ipv4Addresses: { address: string; family: 4 }[] =
          ipv4Response.Answer?.map((answer) => ({
            address: answer.data,
            family: 4,
          })) ?? [];
        const ipv6Addresses: { address: string; family: 6 }[] =
          ipv6Response.Answer?.map((answer) => ({
            address: answer.data,
            family: 6,
          })) ?? [];

        // No addresses found
        if (ipv4Addresses.length === 0 && ipv6Addresses.length === 0) {
          callback(new DnsError(hostname, errorCodes.NOTFOUND, 'queryA'));
          return;
        }

        // Order addresses based on dnsOrder
        if (dnsOrder === 'ipv6first') {
          callback(null, [...ipv6Addresses, ...ipv4Addresses]);
        } else {
          // verbatim - preserve order as received (still separating by type)
          // ipv4first = same as verbatim
          callback(null, [...ipv4Addresses, ...ipv6Addresses]);
        }
      })
      .catch((error: unknown): void => {
        process.nextTick(callback, error);
      });
  } else if (family === 0) {
    // family=0, all=false: query both A and AAAA, return first result based on dnsOrder
    Promise.all([
      sendDnsRequest(hostname, 'A').catch(() => ({ Answer: [] })),
      sendDnsRequest(hostname, 'AAAA').catch(() => ({ Answer: [] })),
    ])
      .then(([ipv4Response, ipv6Response]): void => {
        const ipv4 = ipv4Response.Answer?.at(0)?.data;
        const ipv6 = ipv6Response.Answer?.at(0)?.data;

        if (ipv4 == null && ipv6 == null) {
          callback(new DnsError(hostname, errorCodes.NOTFOUND, 'queryA'));
          return;
        }

        // Return the preferred address based on dnsOrder
        if (dnsOrder === 'ipv6first') {
          if (ipv6 != null) {
            callback(null, ipv6, 6);
          } else {
            callback(null, ipv4 as string, 4);
          }
        } else {
          if (ipv4 != null) {
            callback(null, ipv4, 4);
          } else {
            callback(null, ipv6 as string, 6);
          }
        }
      })
      .catch((error: unknown): void => {
        process.nextTick(callback, error);
      });
  } else {
    const requestType = family === 4 ? 'A' : 'AAAA';

    // Single request when family is specified (with or without all=true)
    sendDnsRequest(hostname, requestType)
      .then((json): void => {
        validateAnswer(json.Answer, hostname, `query${requestType}`);

        if (all) {
          // Return all addresses with the specified family
          callback(
            null,
            json.Answer.map((answer) => ({
              address: answer.data,
              family,
            }))
          );
        } else {
          // Return just the first address
          callback(null, json.Answer.at(0)?.data as string, family);
        }
      })
      .catch((error: unknown): void => {
        process.nextTick(callback, error);
      });
  }
}

// eslint-disable-next-line @typescript-eslint/require-await
export async function lookupService(): Promise<void> {
  // TODO(soon): Implement this.
  throw new Error('Not implemented');
}

export function resolve(
  name: string,
  rrtype: string
): Promise<unknown> | Promise<void> {
  validateString(name, 'name');

  switch (rrtype) {
    case 'A':
      return resolve4(name);
    case 'AAAA':
      return resolve6(name);
    case 'ANY':
      return resolveAny();
    case 'CAA':
      return resolveCaa(name);
    case 'CNAME':
      return resolveCname(name);
    case 'MX':
      return resolveMx(name);
    case 'NAPTR':
      return resolveNaptr(name);
    case 'NS':
      return resolveNs(name);
    case 'PTR':
      return resolvePtr(name);
    case 'SOA':
      return resolveSoa(name);
    case 'SRV':
      return resolveSrv(name);
    case 'TXT':
      return resolveTxt(name);
    default: {
      throw new ERR_INVALID_ARG_VALUE('rrtype', rrtype);
    }
  }
}

export function resolve4(
  name: string,
  options?: { ttl?: boolean }
): Promise<(string | TTLResponse)[]> {
  validateString(name, 'name');

  // The following change is done to comply with Node.js behavior
  const ttl = !!options?.ttl;

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'A').then((json) => {
    validateAnswer(json.Answer, name, 'queryA');

    return json.Answer.map((a) =>
      ttl ? { ttl: a.TTL, address: a.data } : a.data
    );
  });
}

export function resolve6(
  name: string,
  options?: { ttl?: boolean }
): Promise<(string | TTLResponse)[]> {
  validateString(name, 'name');

  // The following change is done to comply with Node.js behavior
  const ttl = !!options?.ttl;

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'AAAA').then((json) => {
    validateAnswer(json.Answer, name, 'queryAaaa');

    return json.Answer.map((a) =>
      ttl ? { ttl: a.TTL, address: a.data } : a.data
    );
  });
}

// eslint-disable-next-line @typescript-eslint/require-await
export async function resolveAny(): Promise<void> {
  // TODO(soon): Implement this
  throw new Error('Not implemented');
}

export function resolveCname(name: string): Promise<string[]> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'CNAME').then((json) => {
    validateAnswer(json.Answer, name, 'queryCname');

    return json.Answer.map(normalizeCname);
  });
}

export function resolveCaa(name: string): Promise<CAA[]> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'CAA').then((json) => {
    validateAnswer(json.Answer, name, 'queryCaa');

    return json.Answer.map(normalizeCaa);
  });
}

export function resolveMx(name: string): Promise<MX[]> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'MX').then((json) => {
    validateAnswer(json.Answer, name, 'queryMx');

    return json.Answer.map((answer) => normalizeMx(name, answer));
  });
}

export function resolveNaptr(name: string): Promise<NAPTR[]> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'NAPTR').then((json) => {
    validateAnswer(json.Answer, name, 'queryNaptr');

    return json.Answer.map(normalizeNaptr);
  });
}

export function resolveNs(name: string): Promise<string[]> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'NS').then((json) => {
    validateAnswer(json.Answer, name, 'queryNs');

    return json.Answer.map(normalizeNs);
  });
}

export function resolvePtr(name: string): Promise<string[]> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'PTR').then((json) => {
    validateAnswer(json.Answer, name, 'queryPtr');

    return json.Answer.map(normalizePtr);
  });
}

export function resolveSoa(name: string): Promise<SOA> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'SOA').then((json) => {
    validateAnswer(json.Answer, name, 'querySoa');

    // This is highly unlikely, but let's assert length just to be safe.
    const firstElement = json.Answer.at(0);
    if (!firstElement) {
      throw new DnsError(name, errorCodes.NOTFOUND, 'querySoa');
    }

    return normalizeSoa(firstElement);
  });
}

export function resolveSrv(name: string): Promise<SRV[]> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'SRV').then((json) => {
    validateAnswer(json.Answer, name, 'querySrv');

    return json.Answer.map(normalizeSrv);
  });
}

export function resolveTxt(name: string): Promise<string[][]> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'TXT').then((json) => {
    validateAnswer(json.Answer, name, 'queryTxt');

    return json.Answer.map(normalizeTxt);
  });
}

export function reverse(name: string): Promise<string[]> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'PTR').then((json) => {
    validateAnswer(json.Answer, name, 'queryPtr');

    return json.Answer.map(({ data }) => {
      if (data.endsWith('.')) {
        return data.slice(0, -1);
      }
      return data;
    });
  });
}

export function setDefaultResultOrder(value: unknown): void {
  validateOneOf(value, 'dnsOrder', validDnsOrders);
  defaultDnsOrder = value as DnsOrder;
}

export function getDefaultResultOrder(): DnsOrder {
  return defaultDnsOrder;
}

export function setServers(): void {
  // This function does not apply to workerd model.
  // Our implementation always use Cloudflare DNS and does not
  // allow users to change the underlying DNS server.
  throw new Error('Not implemented');
}
