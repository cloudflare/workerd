// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

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
import { DnsError } from 'node-internal:internal_errors';
import { validateString } from 'node-internal:validators';
import * as errorCodes from 'node-internal:internal_dns_constants';

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

export function resolve4(
  name: string,
  options?: { ttl?: boolean }
): Promise<(string | TTLResponse)[]> {
  validateString(name, 'name');

  // The following change is done to comply with Node.js behavior
  const ttl = !options?.ttl;

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'A').then((json) => {
    validateAnswer(json.Answer, name, 'queryA');

    return json.Answer.map((a) => {
      if (ttl) {
        return {
          ttl: a.TTL,
          address: a.data,
        };
      }

      return a.data;
    });
  });
}

export function resolve6(
  name: string,
  options?: { ttl?: boolean }
): Promise<(string | TTLResponse)[]> {
  validateString(name, 'name');

  // The following change is done to comply with Node.js behavior
  const ttl = !options?.ttl;

  // Validation errors needs to be sync.
  // Return a promise rather than using async qualifier.
  return sendDnsRequest(name, 'AAAA').then((json) => {
    validateAnswer(json.Answer, name, 'queryAaaa');

    return json.Answer.map((a) => {
      if (ttl) {
        return {
          ttl: a.TTL,
          address: a.data,
        };
      }

      return a.data;
    });
  });
}

export function resolveAny(): void {
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
    if (json.Answer.length === 0) {
      throw new DnsError(name, errorCodes.NOTFOUND, 'querySoa');
    }

    return normalizeSoa(json.Answer[0]!);
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

export function setDefaultResultOrder(): void {
  throw new Error('Not implemented');
}

export function getDefaultResultOrder(): string {
  throw new Error('Not implemented');
}

export function setServers(): void {
  throw new Error('Not implemented');
}
