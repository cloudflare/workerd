// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export function exportKey(key: CryptoKey, options?: ExportOptions): string|Uint8Array|JsonWebKey;
export function equals(key: CryptoKey, otherKey: CryptoKey): boolean;
export function getAsymmetricKeyDetail(key: CryptoKey): AsymmetricKeyDetails;
export function getAsymmetricKeyType(key: CryptoKey): AsymmetricKeyType;
export function generateKeyPair(type: AsymmetricKeyType, options: GenerateKeyPairOptions): CryptoKeyPair;
export function createSecretKey(key: string | ArrayBuffer | Buffer | ArrayBufferView, encoding?: string): CryptoKey;
export function createHmacKey(key: ArrayBufferView): CryptoKey;
export function createPrivateKey(key: CreateAsymmetricKeyOptions |
                                      string |
                                      ArrayBuffer |
                                      ArrayBufferView) : CryptoKey;
export function createPublicKey(key: CreateAsymmetricKeyOptions |
                                     string |
                                     ArrayBuffer |
                                     ArrayBufferView) : CryptoKey;

import { Buffer } from 'node-internal:internal_buffer';

export interface SecretKeyExportOptions {
  format?: 'buffer' | 'jwk';
}

export interface PublicKeyExportOptions {
  type?: 'pkcs1' | 'spki';
  format?: 'pem' | 'der' | 'jwk';
}

export interface PrivateKeyExportOptions {
  type?: 'pkcs1' | 'pkcs8' | 'sec1';
  format?: 'pem' | 'der' | 'jwk';
  cipher?: string;
  passphrase?: string | Uint8Array;
}

export type ExportOptions = SecretKeyExportOptions |
                            PublicKeyExportOptions |
                            PrivateKeyExportOptions;

export interface AsymmetricKeyDetails {
  modulusLength?: number;
  publicExponent?: bigint;
  hashAlgorithm?: string;
  mgf1HashAlgorithm?: string;
  saltLength?: number;
  divisorLength?: number;
  namedCurve?: string;
}

export enum AsymmetricKeyType {
  RSA = 'rsa',
  RSA_PSS = 'rsa-pss',
  DSA = 'dsa',
  EC = 'ec',
  X25519 = 'x25519',
  ED25519 = 'ed25519',
  DH = 'dh',
  // Node.js supports 448 but BoringSSL does not.
  // X448 = 'x448',
  // ED448 = 'ed448',
}

export interface CreateAsymmetricKeyOptions {
  key: string | ArrayBuffer | Buffer | ArrayBufferView | Object;
  format?: 'pem' | 'der' | 'jwk';
  type?: 'pkcs1' | 'pkcs8' | 'sec1' | 'spki';
  passphrase?: string | Uint8Array;
  encoding?: string;
}

export interface GenerateKeyOptions {
  length: number;
}

export interface GenerateKeyPairOptions {
  modulusLength?: number;
  publicExponent?: number|bigint;
  hashAlgorithm?: string;
  mgf1HashAlgorithm?: string;
  saltLength?: number;
  divisorLength?: number;
  namedCurve?: string;
  prime?: Uint8Array;
  primeLength?: number;
  generator?: number;
  groupName?: string;
  paramEncoding?: 'named' | 'explicit';
  publicKeyEncoding?: PublicKeyExportOptions;
  privateKeyEncoding?: PrivateKeyExportOptions;
}

