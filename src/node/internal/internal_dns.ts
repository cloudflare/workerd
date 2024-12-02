// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { sendDnsRequest } from 'node-internal:internal_dns_client';
import { DnsError } from 'node-internal:internal_errors';
import { validateString } from 'node-internal:validators';
import * as errorCodes from 'node-internal:internal_dns_constants';
import { default as dnsUtil } from 'node-internal:dns';

export function getServers(): string[] {
  throw new Error('Not implemented');
}

export function lookup(): void {
  throw new Error('Not implemented');
}

export function lookupService(): void {
  throw new Error('Not implemented');
}

export function resolve(): void {
  throw new Error('Not implemented');
}

export function resolve4(): void {
  throw new Error('Not implemented');
}

export function resolve6(): void {
  throw new Error('Not implemented');
}

export function resolveAny(): void {
  throw new Error('Not implemented');
}

export function resolveCname(): void {
  throw new Error('Not implemented');
}

export function resolveCaa(
  name: string
): Promise<
  { critical: number; issue?: string; iodef?: string; issuewild?: string }[]
> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'CAA').then((json) => {
    if (!('Answer' in json)) {
      // DNS request should contain an "Answer" attribute, but it didn't.
      throw new DnsError(name, errorCodes.NOTFOUND, 'queryTxt');
    }

    // CAA API returns "hex", so we need to convert it to UTF-8
    return json.Answer.map((a) => {
      const obj = { critical: 0 };
      const record = dnsUtil.parseCaaRecord(a.data);
      // eslint-disable-next-line @typescript-eslint/ban-ts-comment
      // @ts-expect-error
      obj[record.field] = record.value;
      obj.critical = record.critical;
      return obj;
    });
  });
}

export function resolveMx(
  name: string
): Promise<{ exchange: string; priority: number }[]> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'MX').then((json) => {
    if (!('Answer' in json)) {
      // DNS request should contain an "Answer" attribute, but it didn't.
      throw new DnsError(name, errorCodes.NOTFOUND, 'queryMx');
    }

    // Cloudflare API returns "data": "10 smtp.google.com." hence
    // we need to parse it.
    return json.Answer.map((a) => {
      const [priority, value]: string[] = a.data.split(' ');
      if (priority == null || value == null) {
        throw new DnsError(name, errorCodes.BADRESP, 'queryMx');
      }
      return {
        exchange: value.slice(0, -1),
        priority: parseInt(priority, 10),
      };
    });
  });
}

export function resolveNaptr(): void {
  throw new Error('Not implemented');
}

export function resolveNs(): void {
  throw new Error('Not implemented');
}

export function resolvePtr(): void {
  throw new Error('Not implemented');
}

export function resolveSoa(): void {
  throw new Error('Not implemented');
}

export function resolveSrv(): void {
  throw new Error('Not implemented');
}

export function resolveTxt(name: string): Promise<string[][]> {
  validateString(name, 'name');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'TXT').then((json) => {
    if (!('Answer' in json)) {
      // DNS request should contain an "Answer" attribute, but it didn't.
      throw new DnsError(name, errorCodes.NOTFOUND, 'queryTxt');
    }

    // Each entry has quotation marks as a prefix and suffix.
    // Node.js APIs doesn't have them.
    return json.Answer.map((value) => [value.data.slice(1, -1)]);
  });
}

export function reverse(input: string): Promise<string[]> {
  validateString(input, 'input');

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(input, 'PTR').then((json) =>
    json.Authority.map((d) => d.data)
  );
}

export function setDefaultResultOrder(): void {
  throw new Error('Not implemented');
}

export function getDefaultResultOrder(): string {
  throw new Error('Not implemented');
}

export function setServers(): void {
  throw new Error('Not implemented');
}
