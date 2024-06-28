// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import {
  Buffer,
} from 'node-internal:internal_buffer';

// random
export function checkPrimeSync(candidate: ArrayBufferView, num_checks: number): boolean;
export function randomPrime(size: number, safe: boolean, add?: ArrayBufferView|undefined,
                            rem?: ArrayBufferView|undefined): ArrayBuffer;

// Hash and Hmac
export class HashHandle {
  public constructor(algorithm: string, xofLen: number);
  public update(data: Buffer | ArrayBufferView): number;
  public digest(): ArrayBuffer;
  public copy(xofLen: number): HashHandle;
}

export type ArrayLike = ArrayBuffer|string|Buffer|ArrayBufferView;

export class HmacHandle {
  public constructor(algorithm: string, key: ArrayLike | CryptoKey);
  public update(data: Buffer | ArrayBufferView): number;
  public digest(): ArrayBuffer;
}

// hkdf
export function getHkdf(hash: string, key: ArrayLike, salt: ArrayLike, info: ArrayLike,
                        length: number): ArrayBuffer;

// pbkdf2
export function getPbkdf(password: ArrayLike, salt: ArrayLike, iterations: number, keylen: number,
                         digest: string): ArrayBuffer;

// scrypt
export function getScrypt(password: ArrayLike, salt: ArrayLike, N: number, r: number, p: number,
                          maxmem: number, keylen: number): ArrayBuffer;

// Keys
export function exportKey(key: CryptoKey, options?: InnerExportOptions): KeyExportResult;
export function equals(key: CryptoKey, otherKey: CryptoKey): boolean;
export function getAsymmetricKeyDetail(key: CryptoKey): AsymmetricKeyDetails;
export function getAsymmetricKeyType(key: CryptoKey): AsymmetricKeyType;
export function createSecretKey(key: ArrayBuffer | ArrayBufferView): CryptoKey;
export function createPrivateKey(key: InnerCreateAsymmetricKeyOptions): CryptoKey;
export function createPublicKey(key: InnerCreateAsymmetricKeyOptions): CryptoKey;

// Spkac
export function verifySpkac(input: ArrayBufferView|ArrayBuffer): boolean;
export function exportPublicKey(input: ArrayBufferView|ArrayBuffer): null | ArrayBuffer;
export function exportChallenge(input: ArrayBufferView|ArrayBuffer): null | ArrayBuffer;

export type KeyData = string | ArrayBuffer | ArrayBufferView;

export interface RsaKeyAlgorithm {
  name: 'rsa' | 'rsa-pss';
  modulusLength: number;
  publicExponent: Uint8Array;
  hash?: string;
}

export interface EcKeyAlgorithm {
  name: 'ec';
  namedCurve: string;
}

export interface DhKeyAlgorithm {
  name: 'dh';
  prime: Uint8Array;
  generator: Uint8Array;
}

export interface DsaKeyAlgorithm {
  name: 'dsa';
  prime: Uint8Array;
  divisorLength: number;
}

export interface HmacKeyAlgorithm {
  name: 'hmac';
  hash: string;
}

export interface AesKeyAlgorithm {
  name: 'aes';
  length: number;
}

export type KeyAlgorithm = RsaKeyAlgorithm |
                           EcKeyAlgorithm |
                           DhKeyAlgorithm |
                           DsaKeyAlgorithm |
                           HmacKeyAlgorithm |
                           AesKeyAlgorithm;

export interface CryptoKey {
  algorithm: KeyAlgorithm;
  extractable: boolean;
  type: KeyObjectType;
  usages: string[];
}

export interface RsaOtherPrimesInfo {
  d?: string;
  r?: string;
  t?: string;
}

export interface JsonWebKey {
  alg?: string;
  crv?: string;
  d?: string;
  dp?: string;
  dq?: string;
  e?: string;
  ext?: boolean;
  k?: string;
  key_ops?: string[];
  kty?: string;
  n?: string;
  oth?: Array<RsaOtherPrimesInfo>;
  p?: string;
  q?: string;
  qi?: string;
  use?: string;
  x?: string;
  y?: string;
}

export interface CryptoKeyPair {
  privateKey: CryptoKey;
  publicKey: CryptoKey;
}

export type KeyObjectType = 'secret' | 'public' | 'private';

export type KeyExportResult = string | Buffer | JsonWebKey;

export type SecretKeyFormat = 'buffer' | 'jwk';
export type AsymmetricKeyFormat = 'pem' | 'der' | 'jwk';
export type PublicKeyEncoding = 'pkcs1' | 'spki';
export type PrivateKeyEncoding = 'pkcs1' | 'pkcs8' | 'sec1';
export type AsymmetricKeyType = 'rsa' | 'rsa-pss' | 'dsa' | 'ec' | 'x25519' | 'ed25519' | 'dh';
export type SecretKeyType = 'hmac' | 'aes';
export type ParamEncoding = 'named' | 'explicit';

export interface SecretKeyExportOptions {
  format?: SecretKeyFormat;
}

export interface PublicKeyExportOptions {
  type?: PublicKeyEncoding;
  format?: AsymmetricKeyFormat;
}

export interface PrivateKeyExportOptions {
  type?: PrivateKeyEncoding;
  format?: AsymmetricKeyFormat;
  cipher?: string;
  passphrase?: string | Uint8Array;
  encoding?: string;
}

export interface InnerPrivateKeyExportOptions {
  type?: PrivateKeyEncoding;
  format?: AsymmetricKeyFormat;
  cipher?: string;
  passphrase?: Uint8Array;
}

export type ExportOptions = SecretKeyExportOptions |
                            PublicKeyExportOptions |
                            PrivateKeyExportOptions;

export type InnerExportOptions = SecretKeyExportOptions |
                                 PublicKeyExportOptions |
                                 InnerPrivateKeyExportOptions;

export interface AsymmetricKeyDetails {
  modulusLength?: number;
  publicExponent?: bigint;
  hashAlgorithm?: string;
  mgf1HashAlgorithm?: string;
  saltLength?: number;
  divisorLength?: number;
  namedCurve?: string;
}

export interface CreateAsymmetricKeyOptions {
  key: string | ArrayBuffer | ArrayBufferView | JsonWebKey;
  format?: AsymmetricKeyFormat;
  type?: PublicKeyEncoding | PrivateKeyEncoding;
  passphrase?: string | Uint8Array;
  encoding?: string;
}

export interface InnerCreateAsymmetricKeyOptions {
  key?: ArrayBuffer | ArrayBufferView | JsonWebKey | CryptoKey;
  format?: AsymmetricKeyFormat;
  type?: PublicKeyEncoding | PrivateKeyEncoding;
  passphrase?: Uint8Array;
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
  paramEncoding?: ParamEncoding;
  publicKeyEncoding?: PublicKeyExportOptions;
  privateKeyEncoding?: PrivateKeyExportOptions;
}

// DiffieHellman
export class DiffieHellmanHandle {
  public constructor(sizeOrKey: number | ArrayBuffer | ArrayBufferView,
                     generator: number | ArrayBuffer | ArrayBufferView);
  public setPublicKey(data: ArrayBuffer | ArrayBufferView | Buffer): void;
  public setPrivateKey(data: ArrayBuffer | ArrayBufferView | Buffer): void;
  public getPublicKey(): ArrayBuffer;
  public getPrivateKey(): ArrayBuffer;
  public getGenerator(): ArrayBuffer;
  public getPrime(): ArrayBuffer;

  public computeSecret(key: ArrayBuffer|ArrayBufferView): ArrayBuffer;
  public generateKeys(): ArrayBuffer;

  public getVerifyError(): number;
}

export function DiffieHellmanGroupHandle(name: string): DiffieHellmanHandle;
