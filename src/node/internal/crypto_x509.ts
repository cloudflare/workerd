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

/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { default as cryptoImpl, CheckOptions } from 'node-internal:crypto';

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

function translatePeerCertificate(c: any) {
  if (!c) return null;

  if (c.issuerCertificate != null && c.issuerCertificate !== c) {
    c.issuerCertificate = translatePeerCertificate(c.issuerCertificate);
  }
  if (c.infoAccess != null) {
    const info = c.infoAccess;
    c.infoAccess = {};

    // XXX: More key validation?
    const regex = /([^\n:]*):([^\n]*)(?:\n|$)/g;
    regex[Symbol.replace](info, (_: any, key: any, val: any): any => {
      if (val.charCodeAt(0) === 0x22) {
        // The translatePeerCertificate function is only
        // used on internally created legacy certificate
        // objects, and any value that contains a quote
        // will always be a valid JSON string literal,
        // so this should never throw.
        val = JSON.parse(val);
      }
      if (key in c.infoAccess) c.infoAccess[key].push(val);
      else c.infoAccess[key] = [val];
    });
  }
  return c;
}

function checkOptions(options?: CheckOptions) {
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
  #state = new Map();

  constructor(
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

  get subject() {
    let value = this.#state.get('subject');
    if (value === undefined) {
      value = this.#handle!.subject;
      this.#state.set('subject', value);
    }
    return value ?? undefined;
  }

  get subjectAltName() {
    let value = this.#state.get('subjectAltName');
    if (value === undefined) {
      value = this.#handle!.subjectAltName;
      this.#state.set('subjectAltName', value);
    }
    return value ?? undefined;
  }

  get issuer() {
    let value = this.#state.get('issuer');
    if (value === undefined) {
      value = this.#handle!.issuer;
      this.#state.set('issuer', value);
    }
    return value ?? undefined;
  }

  get issuerCertificate() {
    let value = this.#state.get('issuerCertificate');
    if (value === undefined) {
      const cert = this.#handle!.issuerCert;
      if (cert) value = new X509Certificate(cert);
      this.#state.set('issuerCertificate', value);
    }
    return value ?? undefined;
  }

  get infoAccess() {
    let value = this.#state.get('infoAccess');
    if (value === undefined) {
      value = this.#handle!.infoAccess;
      this.#state.set('infoAccess', value);
    }
    return value ?? undefined;
  }

  get validFrom() {
    let value = this.#state.get('validFrom');
    if (value === undefined) {
      value = this.#handle!.validFrom;
      this.#state.set('validFrom', value);
    }
    return value ?? undefined;
  }

  get validTo() {
    let value = this.#state.get('validTo');
    if (value === undefined) {
      value = this.#handle!.validTo;
      this.#state.set('validTo', value);
    }
    return value ?? undefined;
  }

  get fingerprint() {
    let value = this.#state.get('fingerprint');
    if (value === undefined) {
      value = this.#handle!.fingerprint;
      this.#state.set('fingerprint', value);
    }
    return value ?? undefined;
  }

  get fingerprint256() {
    let value = this.#state.get('fingerprint256');
    if (value === undefined) {
      value = this.#handle!.fingerprint256;
      this.#state.set('fingerprint256', value);
    }
    return value ?? undefined;
  }

  get fingerprint512() {
    let value = this.#state.get('fingerprint512');
    if (value === undefined) {
      value = this.#handle!.fingerprint512;
      this.#state.set('fingerprint512', value);
    }
    return value ?? undefined;
  }

  get keyUsage() {
    let value = this.#state.get('keyUsage');
    if (value === undefined) {
      value = this.#handle!.keyUsage;
      this.#state.set('keyUsage', value);
    }
    return value ?? undefined;
  }

  get serialNumber() {
    let value = this.#state.get('serialNumber');
    if (value === undefined) {
      value = this.#handle!.serialNumber;
      if (value != null) value = value.toUpperCase();
      this.#state.set('serialNumber', value);
    }
    return value ?? undefined;
  }

  get raw() {
    let value = this.#state.get('raw');
    if (value === undefined) {
      value = this.#handle!.raw;
      if (value != null) value = Buffer.from(value);
      this.#state.set('raw', value);
    }
    return value ?? undefined;
  }

  get publicKey() {
    let value = this.#state.get('publicKey');
    if (value === undefined) {
      const inner = this.#handle!.publicKey;
      if (inner !== undefined) {
        value = PublicKeyObject.from(inner);
        this.#state.set('publicKey', value);
      }
    }
    return value ?? undefined;
  }

  toString() {
    let value = this.#state.get('pem');
    if (value === undefined) {
      value = this.#handle!.pem;
      this.#state.set('pem', value);
    }
    return value ?? undefined;
  }

  // There's no standardized JSON encoding for X509 certs so we
  // fallback to providing the PEM encoding as a string.
  toJSON() {
    return this.toString();
  }

  get ca() {
    let value = this.#state.get('ca');
    if (value === undefined) {
      value = this.#handle!.isCA;
      this.#state.set('ca', value);
    }
    return value ?? false;
  }

  checkHost(name: string, options?: CheckOptions) {
    validateString(name, 'name');
    checkOptions(options);
    return this.#handle!.checkHost(name, options) ?? undefined;
  }

  checkEmail(email: string, options?: CheckOptions) {
    validateString(email, 'email');
    checkOptions(options);
    return this.#handle!.checkEmail(email, options) ?? undefined;
  }

  checkIP(ip: string, options?: CheckOptions) {
    validateString(ip, 'ip');
    checkOptions(options);
    // The options argument is currently undocumented since none of the options
    // have any effect on the behavior of this function. However, we still parse
    // the options argument in case OpenSSL adds flags in the future that do
    // affect the behavior of X509_check_ip. This ensures that no invalid values
    // are passed as the second argument in the meantime.
    return this.#handle!.checkIp(ip, options) ?? undefined;
  }

  checkIssued(otherCert: X509Certificate) {
    if (!(otherCert instanceof X509Certificate))
      throw new ERR_INVALID_ARG_TYPE('otherCert', 'X509Certificate', otherCert);
    return this.#handle!.checkIssued(otherCert.#handle!) ?? undefined;
  }

  checkPrivateKey(pkey: PrivateKeyObject) {
    if (!(pkey instanceof PrivateKeyObject))
      throw new ERR_INVALID_ARG_TYPE('pkey', 'KeyObject', pkey);
    if (pkey.type !== 'private') throw new ERR_INVALID_ARG_VALUE('pkey', pkey);
    return this.#handle!.checkPrivateKey(pkey[kHandle]) ?? undefined;
  }

  verify(pkey: PublicKeyObject) {
    if (!(pkey instanceof PublicKeyObject))
      throw new ERR_INVALID_ARG_TYPE('pkey', 'KeyObject', pkey);
    if (pkey.type !== 'public') throw new ERR_INVALID_ARG_VALUE('pkey', pkey);
    return this.#handle!.verify(pkey[kHandle]);
  }

  toLegacyObject() {
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
      } = this.#handle!.toLegacyObject() as any;
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
