// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { Buffer } from 'node-internal:internal_buffer';

// random
export function checkPrimeSync(
  candidate: ArrayBufferView,
  num_checks: number
): boolean;
export function randomPrime(
  size: number,
  safe: boolean,
  add?: ArrayBufferView,
  rem?: ArrayBufferView
): ArrayBuffer;

export function statelessDH(
  privateKey: CryptoKey,
  publicKey: CryptoKey
): ArrayBuffer;

// X509Certificate
export interface CheckOptions {
  subject?: string;
  wildcards?: boolean;
  partialWildcards?: boolean;
  multiLabelWildcards?: boolean;
  singleLabelSubdomains?: boolean;
}

export class X509Certificate {
  static parse(data: ArrayBuffer | ArrayBufferView): X509Certificate;
  get subject(): string | undefined;
  get subjectAltName(): string | undefined;
  get infoAccess(): string | undefined;
  get issuer(): string | undefined;
  get issuerCert(): X509Certificate | undefined;
  get validFrom(): string | undefined;
  get validTo(): string | undefined;
  get fingerprint(): string | undefined;
  get fingerprint256(): string | undefined;
  get fingerprint512(): string | undefined;
  get keyUsage(): string[] | undefined;
  get serialNumber(): string | undefined;
  get pem(): string | undefined;
  get raw(): ArrayBuffer | undefined;
  get publicKey(): CryptoKey | undefined;
  get isCA(): boolean;
  checkHost(host: string, options?: CheckOptions): string | undefined;
  checkEmail(email: string, options?: CheckOptions): string | undefined;
  checkIp(ip: string, options?: CheckOptions): string | undefined;
  checkIssued(cert: X509Certificate): boolean;
  checkPrivateKey(key: CryptoKey): boolean;
  verify(key: CryptoKey): boolean;
  toLegacyObject(): object;
}

// Hash and Hmac
export class HashHandle {
  constructor(algorithm: string, xofLen: number);
  update(data: Buffer | ArrayBufferView): number;
  digest(): ArrayBuffer;
  copy(xofLen: number): HashHandle;
}

export class SignHandle {
  constructor(algorithm: string);
  update(data: Buffer | ArrayBufferView): void;
  sign(
    key: CryptoKey,
    rsaPadding?: number,
    pssSaltLength?: number,
    dsaSigEnc?: number
  ): Uint8Array;
}

export class VerifyHandle {
  constructor(algorithm: string);
  update(data: Buffer | ArrayBufferView): void;
  verify(
    key: CryptoKey,
    signature: ArrayBufferView,
    rsaPadding?: number,
    pssSaltLength?: number,
    dsaSigEnc?: number
  ): boolean;
}

export function signOneShot(
  key: CryptoKey,
  algorithm: string | undefined,
  data: ArrayBufferView,
  rsaPadding?: number,
  pssSaltLength?: number,
  dsaSigEnc?: number
): ArrayBuffer;
export function verifyOneShot(
  key: CryptoKey,
  algorithm: string | undefined,
  data: ArrayBufferView,
  signature: ArrayBufferView,
  rsaPadding?: number,
  pssSaltLength?: number,
  dsaSigEnc?: number
): boolean;

export class CipherHandle {
  constructor(
    mode: 'cipher' | 'decipher',
    algorithm: string,
    key: CryptoKey,
    iv: ArrayBuffer | ArrayBufferView,
    authTagLength?: number
  );
  update(data: ArrayBuffer | ArrayBufferView): ArrayBuffer;
  final(): ArrayBuffer;
  setAAD(data: ArrayBuffer | ArrayBufferView, plaintextLength?: number): void;
  setAutoPadding(autoPadding: boolean): void;
  getAuthTag(): ArrayBuffer | undefined;
  setAuthTag(tag: ArrayBuffer | ArrayBufferView): void;
}

export interface PublicPrivateCipherOptions {
  padding: number;
  oaepHash: string;
  oaepLabel: ArrayBuffer | ArrayBufferView | undefined;
}

export function publicEncrypt(
  key: CryptoKey,
  buffer: ArrayBuffer | ArrayBufferView,
  options: PublicPrivateCipherOptions
): Buffer;
export function publicDecrypt(
  key: CryptoKey,
  buffer: ArrayBuffer | ArrayBufferView,
  options: PublicPrivateCipherOptions
): Buffer;
export function privateEncrypt(
  key: CryptoKey,
  buffer: ArrayBuffer | ArrayBufferView,
  options: PublicPrivateCipherOptions
): Buffer;
export function privateDecrypt(
  key: CryptoKey,
  buffer: ArrayBuffer | ArrayBufferView,
  options: PublicPrivateCipherOptions
): Buffer;

interface CipherInfo {
  name: string;
  nid: number;
  blockSize?: number;
  ivLength?: number;
  keyLength: number;
  mode:
    | 'cbc'
    | 'ccm'
    | 'cfb'
    | 'ctr'
    | 'ecb'
    | 'gcm'
    | 'ocb'
    | 'ofb'
    | 'stream'
    | 'wrap'
    | 'xts';
}

interface GetCipherInfoOptions {
  ivLength?: number;
  keyLength?: number;
}

export function getCipherInfo(
  nameOrId: string | number,
  options: GetCipherInfoOptions
): CipherInfo | undefined;

export type ArrayLike = ArrayBuffer | string | Buffer | ArrayBufferView;

export class HmacHandle {
  constructor(algorithm: string, key: ArrayLike | CryptoKey);
  update(data: Buffer | ArrayBufferView): number;
  digest(): ArrayBuffer;
}

// hkdf
export function getHkdf(
  hash: string,
  key: ArrayLike,
  salt: ArrayLike,
  info: ArrayLike,
  length: number
): ArrayBuffer;

// pbkdf2
export function getPbkdf(
  password: ArrayLike,
  salt: ArrayLike,
  iterations: number,
  keylen: number,
  digest: string
): ArrayBuffer;

// scrypt
export function getScrypt(
  password: ArrayLike,
  salt: ArrayLike,
  N: number,
  r: number,
  p: number,
  maxmem: number,
  keylen: number
): ArrayBuffer;

// Keys
export function exportKey(
  key: CryptoKey,
  options?: InnerExportOptions
): KeyExportResult;
export function equals(key: CryptoKey, otherKey: CryptoKey): boolean;
export function getAsymmetricKeyDetail(key: CryptoKey): AsymmetricKeyDetails;
export function getAsymmetricKeyType(key: CryptoKey): AsymmetricKeyType;
export function createSecretKey(key: ArrayBuffer | ArrayBufferView): CryptoKey;
export function createPrivateKey(
  key: InnerCreateAsymmetricKeyOptions
): CryptoKey;
export function createPublicKey(
  key: InnerCreateAsymmetricKeyOptions
): CryptoKey;

export interface RsaKeyPairOptions {
  type: string;
  modulusLength: number;
  publicExponent: number;
  saltLength?: number;
  hashAlgorithm?: string;
  mgf1HashAlgorithm?: string;
}

export interface DsaKeyPairOptions {
  modulusLength: number;
  divisorLength: number;
}

export interface EcKeyPairOptions {
  namedCurve: string;
  paramEncoding: ParamEncoding;
}

export interface EdKeyPairOptions {
  type: string;
}

export interface DhKeyPairOptions {
  primeOrGroup: BufferSource | number | string;
  generator?: number | undefined;
}

export function generateRsaKeyPair(options: RsaKeyPairOptions): CryptoKeyPair;
export function generateDsaKeyPair(options: DsaKeyPairOptions): CryptoKeyPair;
export function generateEcKeyPair(options: EcKeyPairOptions): CryptoKeyPair;
export function generateEdKeyPair(options: EdKeyPairOptions): CryptoKeyPair;
export function generateDhKeyPair(options: DhKeyPairOptions): CryptoKeyPair;

// Spkac
export function verifySpkac(input: ArrayBufferView | ArrayBuffer): boolean;
export function exportPublicKey(
  input: ArrayBufferView | ArrayBuffer
): null | ArrayBuffer;
export function exportChallenge(
  input: ArrayBufferView | ArrayBuffer
): null | ArrayBuffer;

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

export type KeyAlgorithm =
  | RsaKeyAlgorithm
  | EcKeyAlgorithm
  | DhKeyAlgorithm
  | DsaKeyAlgorithm
  | HmacKeyAlgorithm
  | AesKeyAlgorithm;

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
export type AsymmetricKeyType = 'rsa' | 'ec' | 'x25519' | 'ed25519' | 'dh';
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

export type ExportOptions =
  | SecretKeyExportOptions
  | PublicKeyExportOptions
  | PrivateKeyExportOptions;

export type InnerExportOptions =
  | SecretKeyExportOptions
  | PublicKeyExportOptions
  | InnerPrivateKeyExportOptions;

export interface AsymmetricKeyDetails {
  modulusLength?: number;
  publicExponent?: bigint;
  hashAlgorithm?: string;
  mgf1HashAlgorithm?: string;
  saltLength?: number;
  divisorLength?: number;
  namedCurve?: string;
}

// The user-provided options passed to createPrivateKey or createPublicKey.
// This will be processed into an InnerCreateAsymmetricKeyOptions.
export interface CreateAsymmetricKeyOptions {
  key: string | ArrayBuffer | ArrayBufferView | JsonWebKey | null | undefined;
  format?: AsymmetricKeyFormat;
  type?: PublicKeyEncoding | PrivateKeyEncoding;
  passphrase?: string | Uint8Array | Buffer;
  encoding?: string;
}

// The processed key options. The key property will be one of either
// an ArrayBuffer, an ArrayBufferView, a JWK, or a CryptoKey. The
// format and type options will be validated to known good values,
// and the passphrase will either be undefined or an ArrayBufferView.
export interface InnerCreateAsymmetricKeyOptions {
  // CryptoKey is only used when importing a public key derived from
  // an existing private key.
  key: ArrayBuffer | ArrayBufferView | JsonWebKey | CryptoKey;
  format: AsymmetricKeyFormat;
  type: PublicKeyEncoding | PrivateKeyEncoding | undefined;
  passphrase: Buffer | ArrayBuffer | ArrayBufferView | undefined;
}

export interface GenerateKeyOptions {
  length: number;
}

export interface GenerateKeyPairOptions {
  modulusLength?: number;
  publicExponent?: number | bigint;
  hash?: string;
  hashAlgorithm?: string;
  mgf1Hash?: string;
  mgf1HashAlgorithm?: string;
  saltLength?: number;
  divisorLength?: number;
  namedCurve?: string;
  prime?: Uint8Array;
  primeLength?: number;
  generator?: number;
  group?: string;
  groupName?: string;
  paramEncoding?: ParamEncoding;
  publicKeyEncoding?: PublicKeyExportOptions;
  privateKeyEncoding?: PrivateKeyExportOptions;
}

// DiffieHellman
export class DiffieHellmanHandle {
  constructor(
    sizeOrKey: number | ArrayBuffer | ArrayBufferView,
    generator: number | ArrayBuffer | ArrayBufferView
  );
  setPublicKey(data: ArrayBuffer | ArrayBufferView | Buffer): void;
  setPrivateKey(data: ArrayBuffer | ArrayBufferView | Buffer): void;
  getPublicKey(): ArrayBuffer;
  getPrivateKey(): ArrayBuffer;
  getGenerator(): ArrayBuffer;
  getPrime(): ArrayBuffer;

  computeSecret(key: ArrayBuffer | ArrayBufferView): ArrayBuffer;
  generateKeys(): ArrayBuffer;

  getVerifyError(): number;
}

export type ECDHFormat = 'compressed' | 'uncompressed' | 'hybrid';
export class ECDHHandle {
  constructor(curveName: string);
  computeSecret(otherPublicKey: ArrayBufferView): ArrayBuffer;
  generateKeys(): ArrayBuffer;
  getPrivateKey(): ArrayBuffer;
  getPublicKey(format: ECDHFormat): ArrayBuffer;
  setPrivateKey(key: ArrayBufferView): void;
  static convertKey(
    key: ArrayBufferView,
    curveName: string,
    format: ECDHFormat
  ): ArrayBuffer;
}

export function DiffieHellmanGroupHandle(name: string): DiffieHellmanHandle;
