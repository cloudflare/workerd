// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { sendDnsRequest } from 'node-internal:internal_dns_client';
import { DnsError } from 'node-internal:internal_errors';
import { validateString } from 'node-internal:validators';
import * as errorCodes from 'node-internal:internal_dns_constants';
import { Buffer } from 'node-internal:internal_buffer';

function hexToUtf8(input: string): { name: string; value: string } {
  // Example input: '\\# 15 00 05 69 73 73 75 65 70 6b 69 2e 67 6f 6f 67'
  // Input starts with "\\#"
  const hex: string = input.slice(3).replaceAll(' ', '');
  const buffer: string = Buffer.from(hex, 'hex').toString('utf8').slice(3);

  // TODO(soon): Implement a better parser.
  if (buffer.startsWith('iodef')) {
    return {
      name: 'iodef',
      value: buffer.slice(5),
    };
  } else if (buffer.startsWith('issue')) {
    return {
      name: 'issue',
      value: buffer.slice(5),
    };
  } else if (buffer.startsWith('issuewild')) {
    return {
      name: 'issuewild',
      value: buffer.slice(9),
    };
  }
  throw new Error(`Invalid HEX prefix "${buffer}"`);
}

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
  return sendDnsRequest(name, 'CAA').then((json) => {
    if (!('Answer' in json)) {
      // DNS request should contain an "Answer" attribute, but it didn't.
      throw new DnsError(name, errorCodes.NOTFOUND, 'queryTxt');
    }

    // CAA API returns "hex", so we need to convert it to UTF-8
    return json.Answer.map((a) => {
      const obj = { critical: 0 };
      const sanitized = hexToUtf8(a.data);
      // eslint-disable-next-line @typescript-eslint/ban-ts-comment
      // @ts-expect-error
      obj[sanitized.name] = sanitized.value;
      return obj;
    });
  });
}

export function resolveMx(
  name: string
): Promise<{ exchange: string; priority: number }[]> {
  validateString(name, 'name');
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
