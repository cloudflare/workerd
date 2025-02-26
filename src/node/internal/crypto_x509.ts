// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import {
  default as cryptoImpl,
  CheckOptions,
  type KeyObjectType,
} from 'node-internal:crypto';

import {
  validateString,
  validateObject,
  validateBoolean,
} from 'node-internal:validators';

import { isArrayBufferView } from 'node-internal:internal_types';

import { Buffer } from 'node-internal:internal_buffer';

import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
} from 'node-internal:internal_errors';

import { kHandle } from 'node-internal:crypto_util';

import { PublicKeyObject, PrivateKeyObject } from 'node-internal:crypto_keys';

function translatePeerCertificate(c?: X509Certificate): X509Certificate | null {
  if (!c) {
    return null;
  }

  if (c.issuerCertificate != null && c.issuerCertificate !== c) {
    c.issuerCertificate = translatePeerCertificate(c.issuerCertificate);
  }
  if (c.infoAccess != null) {
    const info = c.infoAccess;
    c.infoAccess = {};

    // XXX: More key validation?
    const regex = /([^\n:]*):([^\n]*)(?:\n|$)/g;
    regex[Symbol.replace](
      // @ts-expect-error TS2769 Investigate this.
      info,
      (_: unknown, key: string, val: string): void => {
        let parsedVal: unknown = val;
        if (val.charCodeAt(0) === 0x22) {
          // The translatePeerCertificate function is only
          // used on internally created legacy certificate
          // objects, and any value that contains a quote
          // will always be a valid JSON string literal,
          // so this should never throw.
          parsedVal = JSON.parse(val) as unknown;
        }
        if (c.infoAccess) {
          if (key in c.infoAccess) {
            (c.infoAccess[key] as unknown[]).push(parsedVal);
          } else {
            c.infoAccess[key] = [parsedVal];
          }
        }
      }
    );
  }
  return c;
}

function checkOptions(options?: CheckOptions): void {
  if (options == null) return;
  validateObject(options, 'options');
  if (options.multiLabelWildcards !== undefined)
    validateBoolean(options.multiLabelWildcards, 'options.multiLabelWildcards');
  if (options.partialWildcards !== undefined)
    validateBoolean(options.partialWildcards, 'options.partialWildcards');
  if (options.singleLabelSubdomains !== undefined)
    validateBoolean(
      options.singleLabelSubdomains,
      'options.singleLabelSubdomains'
    );
  if (options.wildcards !== undefined)
    validateBoolean(options.wildcards, 'options.wildcards');
  if (options.subject !== undefined)
    validateString(options.subject, 'options.subject');
}

export class X509Certificate {
  #handle?: cryptoImpl.X509Certificate = undefined;
  #state = new Map<string, unknown>();

  public constructor(
    buffer: ArrayBufferView | ArrayBuffer | cryptoImpl.X509Certificate | string
  ) {
    if (buffer instanceof cryptoImpl.X509Certificate) {
      this.#handle = buffer;
      return;
    }
    if (typeof buffer === 'string') {
      buffer = Buffer.from(buffer);
    }
    if (!isArrayBufferView(buffer)) {
      throw new ERR_INVALID_ARG_TYPE(
        'buffer',
        ['string', 'Buffer', 'TypedArray', 'DataView'],
        buffer
      );
    }
    this.#handle = cryptoImpl.X509Certificate.parse(buffer);
  }

  public get subject(): string | undefined {
    let value = this.#state.get('subject') as string | undefined;
    if (value === undefined) {
      value = this.#handle?.subject;
      this.#state.set('subject', value);
    }
    return value ?? undefined;
  }

  public get subjectAltName(): string | undefined {
    let value = this.#state.get('subjectAltName') as string | undefined;
    if (value === undefined) {
      value = this.#handle?.subjectAltName;
      this.#state.set('subjectAltName', value);
    }
    return value ?? undefined;
  }

  public get issuer(): string | undefined {
    let value = this.#state.get('issuer') as string | undefined;
    if (value === undefined) {
      value = this.#handle?.issuer;
      this.#state.set('issuer', value);
    }
    return value ?? undefined;
  }

  public get issuerCertificate(): X509Certificate | undefined {
    let value = this.#state.get('issuerCertificate') as
      | X509Certificate
      | undefined;
    if (value === undefined) {
      const cert = this.#handle?.issuerCert;
      if (cert) value = new X509Certificate(cert);
      this.#state.set('issuerCertificate', value);
    }
    return value ?? undefined;
  }

  public set issuerCertificate(value: unknown) {
    this.#state.set('issuerCertificate', value);
  }

  public get infoAccess(): Record<string, unknown> | undefined {
    let value = this.#state.get('infoAccess') as
      | Record<string, unknown>
      | undefined;
    if (value === undefined) {
      value = this.#handle?.infoAccess as unknown as
        | Record<string, unknown>
        | undefined;
      this.#state.set('infoAccess', value);
    }
    return value ?? undefined;
  }

  public set infoAccess(value: unknown) {
    this.#state.set('infoAccess', value);
  }

  public get validFrom(): string | undefined {
    let value = this.#state.get('validFrom') as string | undefined;
    if (value === undefined) {
      value = this.#handle?.validFrom;
      this.#state.set('validFrom', value);
    }
    return value ?? undefined;
  }

  public get validTo(): string | undefined {
    let value = this.#state.get('validTo') as string | undefined;
    if (value === undefined) {
      value = this.#handle?.validTo;
      this.#state.set('validTo', value);
    }
    return value ?? undefined;
  }

  public get fingerprint(): string | undefined {
    let value = this.#state.get('fingerprint') as string | undefined;
    if (value === undefined) {
      value = this.#handle?.fingerprint;
      this.#state.set('fingerprint', value);
    }
    return value ?? undefined;
  }

  public get fingerprint256(): string | undefined {
    let value = this.#state.get('fingerprint256') as string | undefined;
    if (value === undefined) {
      value = this.#handle?.fingerprint256;
      this.#state.set('fingerprint256', value);
    }
    return value ?? undefined;
  }

  public get fingerprint512(): string | undefined {
    let value = this.#state.get('fingerprint512') as string | undefined;
    if (value === undefined) {
      value = this.#handle?.fingerprint512;
      this.#state.set('fingerprint512', value);
    }
    return value ?? undefined;
  }

  public get keyUsage(): string[] | undefined {
    let value = this.#state.get('keyUsage') as string[] | undefined;
    if (value === undefined) {
      value = this.#handle?.keyUsage;
      this.#state.set('keyUsage', value);
    }
    return value ?? undefined;
  }

  public get serialNumber(): string | undefined {
    let value = this.#state.get('serialNumber') as string | undefined;
    if (value === undefined) {
      value = this.#handle?.serialNumber;
      if (value != null) value = value.toUpperCase();
      this.#state.set('serialNumber', value);
    }
    return value ?? undefined;
  }

  public get raw(): ArrayBuffer | undefined {
    let value = this.#state.get('raw') as ArrayBuffer | undefined;
    if (value === undefined) {
      value = this.#handle?.raw;
      if (value != null) value = Buffer.from(value);
      this.#state.set('raw', value);
    }
    return value ?? undefined;
  }

  public get publicKey(): KeyObjectType | undefined {
    let value = this.#state.get('publicKey') as KeyObjectType | undefined;
    if (value === undefined) {
      const inner = this.#handle?.publicKey;
      if (inner !== undefined) {
        value = PublicKeyObject.from(inner);
        this.#state.set('publicKey', value);
      }
    }
    return value ?? undefined;
  }

  public toString(): string | undefined {
    let value = this.#state.get('pem') as string | undefined;
    if (value === undefined) {
      value = this.#handle?.pem;
      this.#state.set('pem', value);
    }
    return value ?? undefined;
  }

  // There's no standardized JSON encoding for X509 certs so we
  // fallback to providing the PEM encoding as a string.
  public toJSON(): string | undefined {
    return this.toString();
  }

  public get ca(): boolean {
    let value = this.#state.get('ca') as boolean | undefined | null;
    if (value === undefined) {
      value = this.#handle?.isCA;
      this.#state.set('ca', value);
    }
    return value ?? false;
  }

  public checkHost(name: string, options?: CheckOptions): string | undefined {
    validateString(name, 'name');
    checkOptions(options);
    return this.#handle?.checkHost(name, options) ?? undefined;
  }

  public checkEmail(email: string, options?: CheckOptions): string | undefined {
    validateString(email, 'email');
    checkOptions(options);
    return this.#handle?.checkEmail(email, options) ?? undefined;
  }

  public checkIP(ip: string, options?: CheckOptions): string | undefined {
    validateString(ip, 'ip');
    checkOptions(options);
    // The options argument is currently undocumented since none of the options
    // have any effect on the behavior of this function. However, we still parse
    // the options argument in case OpenSSL adds flags in the future that do
    // affect the behavior of X509_check_ip. This ensures that no invalid values
    // are passed as the second argument in the meantime.
    return this.#handle?.checkIp(ip, options) ?? undefined;
  }

  public checkIssued(otherCert: X509Certificate): boolean | undefined {
    if (!(otherCert instanceof X509Certificate))
      throw new ERR_INVALID_ARG_TYPE('otherCert', 'X509Certificate', otherCert);
    return this.#handle?.checkIssued(otherCert.#handle!) ?? undefined;
  }

  public checkPrivateKey(pkey: PrivateKeyObject): boolean | undefined {
    if (!(pkey instanceof PrivateKeyObject))
      throw new ERR_INVALID_ARG_TYPE('pkey', 'KeyObject', pkey);
    if (pkey.type !== 'private') throw new ERR_INVALID_ARG_VALUE('pkey', pkey);
    return this.#handle?.checkPrivateKey(pkey[kHandle]) ?? undefined;
  }

  public verify(pkey: PublicKeyObject): boolean {
    if (!(pkey instanceof PublicKeyObject))
      throw new ERR_INVALID_ARG_TYPE('pkey', 'KeyObject', pkey);
    if (pkey.type !== 'public') throw new ERR_INVALID_ARG_VALUE('pkey', pkey);
    return this.#handle?.verify(pkey[kHandle]) ?? false;
  }

  public toLegacyObject(): unknown {
    let value = this.#state.get('legacy');
    if (value === undefined) {
      let {
        subject,
        subjectAltName,
        infoAccess,
        issuer,
        ca,
        modulus,
        bits,
        exponent,
        pubkey,
        asn1Curve,
        nistCurve,
        valid_from,
        valid_to,
        fingerprint,
        fingerprint256,
        fingerprint512,
        serialNumber,
        ext_key_usage,
        raw,
      } = this.#handle?.toLegacyObject() ?? {};
      if (raw != null) raw = Buffer.from(raw);
      if (pubkey != null) pubkey = Buffer.from(pubkey);
      if (modulus != null) modulus = modulus.toUpperCase();
      if (fingerprint != null) fingerprint = fingerprint.toUpperCase();
      if (fingerprint256 != null) fingerprint256 = fingerprint256.toUpperCase();
      if (fingerprint512 != null) fingerprint512 = fingerprint512.toUpperCase();
      if (serialNumber != null) serialNumber = serialNumber.toUpperCase();
      value = translatePeerCertificate({
        subject,
        subjectAltName,
        infoAccess,
        issuer,
        ca,
        modulus,
        bits,
        exponent,
        pubkey,
        asn1Curve,
        nistCurve,
        valid_from,
        valid_to,
        fingerprint,
        fingerprint256,
        fingerprint512,
        serialNumber,
        ext_key_usage,
        raw,
      });
      this.#state.set('legacy', value);
    }
    return value;
  }
}
