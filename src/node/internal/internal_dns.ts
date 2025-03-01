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

// eslint-disable-next-line @typescript-eslint/require-await
export async function getServers(): Promise<string[]> {
  return ['1.1.1.1', '2606:4700:4700::1111', '1.0.0.1', '2606:4700:4700::1001'];
}

// eslint-disable-next-line @typescript-eslint/require-await
export async function lookup(): Promise<void> {
  // TODO(soon): Implement this.
  throw new Error('Not implemented');
}

// eslint-disable-next-line @typescript-eslint/require-await
export async function lookupService(): Promise<void> {
  // TODO(soon): Implement this.
  throw new Error('Not implemented');
}

// eslint-disable-next-line @typescript-eslint/require-await
export async function resolve(): Promise<void> {
  // TODO(soon): Implement this.
  throw new Error('Not implemented');
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

// eslint-disable-next-line @typescript-eslint/require-await
export async function setDefaultResultOrder(): Promise<void> {
  // Does not apply to workerd
  throw new Error('Not implemented');
}

// eslint-disable-next-line @typescript-eslint/require-await
export async function getDefaultResultOrder(): Promise<void> {
  // Does not apply to workerd
  throw new Error('Not implemented');
}

// eslint-disable-next-line @typescript-eslint/require-await
export async function setServers(): Promise<void> {
  // This function does not apply to workerd model.
  // Our implementation always use Cloudflare DNS and does not
  // allow users to change the underlying DNS server.
  throw new Error('Not implemented');
}
