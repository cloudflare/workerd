// Copyright (c) 2017-2023 Cloudflare, Inc.
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
  PublicPrivateCipherOptions,
  type CipherHandle,
} from 'node-internal:crypto';

import {
  Transform,
  TransformOptions,
  TransformCallback,
} from 'node-internal:streams_transform';

import {
  type KeyObject,
  createSecretKey,
  createPublicKey,
  createPrivateKey,
  isKeyObject,
  getKeyObjectHandle,
} from 'node-internal:crypto_keys';

import {
  validateNumber,
  validateObject,
  validateString,
  validateUint32,
} from 'node-internal:validators';

import {
  ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_MISSING_ARGS,
} from 'node-internal:internal_errors';

import {
  isAnyArrayBuffer,
  isArrayBufferView,
} from 'node-internal:internal_types';

import {
  type KeyData,
  type CreateAsymmetricKeyOptions,
} from 'node-internal:crypto';

import { Buffer } from 'node-internal:internal_buffer';

export interface AADOptions {
  plaintextLength?: number;
  encoding?: string;
}

const kHandle = Symbol('kHandle');

export interface Cipheriv extends Transform {
  [kHandle]: CipherHandle;
  update(
    data: string | ArrayBuffer | ArrayBufferView,
    inputEncoding?: string,
    outputEncoding?: string
  ): string | Buffer;
  final(outputEncoding?: string): string | Buffer;
  setAAD(
    buffer: string | ArrayBuffer | ArrayBufferView,
    options?: AADOptions
  ): this;
  setAutoPadding(autoPadding?: boolean): this;
  getAuthTag(): Buffer | undefined;
}

export interface Decipheriv extends Transform {
  [kHandle]: CipherHandle;
  update(
    data: string | ArrayBuffer | ArrayBufferView,
    inputEncoding?: string,
    outputEncoding?: string
  ): string | Buffer;
  final(outputEncoding?: string): string | Buffer;
  setAAD(
    buffer: string | ArrayBuffer | ArrayBufferView,
    options?: AADOptions
  ): this;
  setAutoPadding(autoPadding?: boolean): this;
  setAuthTag(buffer: ArrayBuffer | ArrayBufferView): this;
  setAuthTag(buffer: string, encoding?: string): this;
}

export interface CipherOptions extends TransformOptions {
  authLengthTag?: number;
}

function getSecretKey(
  key: string | ArrayBuffer | ArrayBufferView | KeyObject | CryptoKey
): CryptoKey {
  if (key instanceof CryptoKey) {
    if (key.type !== 'secret') {
      throw new ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE(key.type, 'secret');
    }
    return key;
  } else if (isKeyObject(key)) {
    if (key.type !== 'secret') {
      throw new ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE(key.type, 'secret');
    }
    return getKeyObjectHandle(key);
  } else if (isAnyArrayBuffer(key) || isArrayBufferView(key)) {
    return getKeyObjectHandle(createSecretKey(key));
  } else if (typeof key === 'string') {
    return getKeyObjectHandle(createSecretKey(key, 'utf8'));
  }

  throw new ERR_INVALID_ARG_TYPE(
    'key',
    ['string', 'Buffer', 'TypedArray', 'DataView', 'KeyObject', 'CryptoKey'],
    key
  );
}

function getIv(
  iv: string | ArrayBuffer | ArrayBufferView | null
): ArrayBuffer | ArrayBufferView {
  if (isAnyArrayBuffer(iv) || isArrayBufferView(iv)) {
    return iv;
  } else if (typeof iv === 'string') {
    return Buffer.from(iv, 'utf8');
  }

  throw new ERR_INVALID_ARG_TYPE(
    'iv',
    ['string', 'Buffer', 'TypedArray', 'DataView', 'null'],
    iv
  );
}

export const Cipheriv = function (
  this: Cipheriv,
  algorithm: string,
  key: string | ArrayBuffer | ArrayBufferView | KeyObject | CryptoKey,
  iv: string | ArrayBuffer | ArrayBufferView | null,
  options: CipherOptions = {}
) {
  validateString(algorithm, 'algorithm');
  const secretKey = getSecretKey(key);
  const ivBuf = getIv(iv);

  if (options.authLengthTag !== undefined) {
    validateUint32(options.authLengthTag, 'options.authLengthTag');
  }

  this[kHandle] = new cryptoImpl.CipherHandle(
    'cipher',
    algorithm,
    secretKey,
    ivBuf,
    options.authLengthTag
  );

  Transform.call(this, options as TransformOptions);
} as unknown as {
  new (
    algorithm: string,
    key: string | ArrayBuffer | ArrayBufferView | KeyObject | CryptoKey,
    iv: string | ArrayBuffer | ArrayBufferView | null,
    options?: TransformOptions
  ): Cipheriv;
};
Object.setPrototypeOf(Cipheriv.prototype, Transform.prototype);
Object.setPrototypeOf(Cipheriv, Transform);

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Cipheriv.prototype.update = function (
  this: Cipheriv,
  data: string | ArrayBuffer | ArrayBufferView,
  inputEncoding?: string,
  outputEncoding?: string
): string | Buffer {
  let ret: ArrayBuffer;
  if (typeof data === 'string') {
    if (inputEncoding === undefined) {
      throw new ERR_INVALID_ARG_VALUE(
        'inputEncoding',
        inputEncoding,
        'If inputEncoding is not provided then the data must be a Buffer'
      );
    }
    ret = this[kHandle].update(Buffer.from(data, inputEncoding));
  } else if (isAnyArrayBuffer(data)) {
    ret = this[kHandle].update(data);
  } else if (isArrayBufferView(data)) {
    ret = this[kHandle].update(data);
  } else {
    throw new ERR_INVALID_ARG_TYPE(
      'data',
      ['string', 'Buffer', 'TypedArray', 'DataView'],
      data
    );
  }

  if (outputEncoding === undefined) {
    return Buffer.from(ret);
  }

  return Buffer.from(ret).toString(outputEncoding);
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Cipheriv.prototype.final = function (
  this: Cipheriv,
  outputEncoding?: string
): string | Buffer {
  const ret = this[kHandle].final();
  if (outputEncoding === undefined) {
    return Buffer.from(ret);
  }
  return Buffer.from(ret).toString(outputEncoding);
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Cipheriv.prototype.setAAD = function (
  this: Cipheriv,
  buffer: string | ArrayBuffer | ArrayBufferView,
  options: AADOptions = {}
): Cipheriv {
  validateObject(options, 'options');
  const { plaintextLength, encoding } = options;
  if (plaintextLength !== undefined) {
    validateUint32(plaintextLength, 'options.plaintextLength');
  }
  if (encoding !== undefined) {
    validateString(encoding, 'options.encoding');
  }

  if (typeof buffer === 'string') {
    this[kHandle].setAAD(Buffer.from(buffer, encoding), plaintextLength);
  } else if (isAnyArrayBuffer(buffer) || isArrayBufferView(buffer)) {
    this[kHandle].setAAD(buffer, plaintextLength);
  }
  return this;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Cipheriv.prototype.setAutoPadding = function (
  this: Cipheriv,
  autoPadding?: boolean
): Cipheriv {
  this[kHandle].setAutoPadding(autoPadding ? true : false);
  return this;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Cipheriv.prototype.getAuthTag = function (this: Cipheriv): Buffer | undefined {
  const ret = this[kHandle].getAuthTag();
  if (ret === undefined) return undefined;
  return Buffer.from(ret);
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Cipheriv.prototype._transform = function (
  this: Cipheriv,
  chunk: string | Buffer | ArrayBufferView,
  encoding: string,
  callback: TransformCallback
): void {
  if (typeof chunk === 'string') {
    chunk = Buffer.from(chunk, encoding);
  }
  this.push(Buffer.from(this[kHandle].update(chunk)));
  callback();
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Cipheriv.prototype._flush = function (
  this: Cipheriv,
  callback: TransformCallback
): void {
  this.push(Buffer.from(this[kHandle].final()));
  callback();
};

export const Decipheriv = function (
  this: Decipheriv,
  algorithm: string,
  key: string | ArrayBuffer | ArrayBufferView | KeyObject | CryptoKey,
  iv: string | ArrayBuffer | ArrayBufferView | null,
  options: CipherOptions = {}
) {
  validateString(algorithm, 'algorithm');
  const secretKey = getSecretKey(key);
  const ivBuf = getIv(iv);

  if (options.authLengthTag !== undefined) {
    validateUint32(options.authLengthTag, 'options.authLengthTag');
  }

  this[kHandle] = new cryptoImpl.CipherHandle(
    'decipher',
    algorithm,
    secretKey,
    ivBuf,
    options.authLengthTag
  );

  Transform.call(this, options as TransformOptions);
  return this;
} as unknown as {
  new (
    algorithm: string,
    key: string | ArrayBuffer | ArrayBufferView | KeyObject | CryptoKey,
    iv: string | ArrayBuffer | ArrayBufferView | null,
    options?: TransformOptions
  ): Decipheriv;
};

Object.setPrototypeOf(Decipheriv.prototype, Transform.prototype);
Object.setPrototypeOf(Decipheriv, Transform);

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Decipheriv.prototype.update = function (
  this: Decipheriv,
  data: string | ArrayBuffer | ArrayBufferView,
  inputEncoding?: string,
  outputEncoding?: string
): string | Buffer {
  let ret: ArrayBuffer;
  if (typeof data === 'string') {
    if (inputEncoding === undefined) {
      throw new ERR_INVALID_ARG_VALUE(
        'inputEncoding',
        inputEncoding,
        'If inputEncoding is not provided then the data must be a Buffer'
      );
    }
    ret = this[kHandle].update(Buffer.from(data, inputEncoding));
  } else if (isAnyArrayBuffer(data)) {
    ret = this[kHandle].update(data);
  } else if (isArrayBufferView(data)) {
    ret = this[kHandle].update(data);
  } else {
    throw new ERR_INVALID_ARG_TYPE(
      'data',
      ['string', 'Buffer', 'TypedArray', 'DataView'],
      data
    );
  }

  if (outputEncoding === undefined) {
    return Buffer.from(ret);
  }

  return Buffer.from(ret).toString(outputEncoding);
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Decipheriv.prototype.final = function (
  this: Decipheriv,
  outputEncoding?: string
): string | Buffer {
  const ret = this[kHandle].final();
  if (outputEncoding === undefined) {
    return Buffer.from(ret);
  }
  return Buffer.from(ret).toString(outputEncoding);
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Decipheriv.prototype.setAAD = function (
  this: Decipheriv,
  buffer: string | ArrayBuffer | ArrayBufferView,
  options: AADOptions = {}
): Decipheriv {
  validateObject(options, 'options');
  const { plaintextLength, encoding } = options;
  if (plaintextLength !== undefined) {
    validateUint32(plaintextLength, 'options.plaintextLength');
  }
  if (encoding !== undefined) {
    validateString(encoding, 'options.encoding');
  }

  if (typeof buffer === 'string') {
    this[kHandle].setAAD(Buffer.from(buffer, encoding), plaintextLength);
  } else if (isAnyArrayBuffer(buffer) || isArrayBufferView(buffer)) {
    this[kHandle].setAAD(buffer, plaintextLength);
  }
  return this;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Decipheriv.prototype.setAutoPadding = function (
  this: Decipheriv,
  autoPadding?: boolean
): Decipheriv {
  this[kHandle].setAutoPadding(autoPadding ? true : false);
  return this;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Decipheriv.prototype.setAuthTag = function (
  this: Decipheriv,
  buffer: ArrayBuffer | ArrayBufferView
): Decipheriv {
  this[kHandle].setAuthTag(buffer);
  return this;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Decipheriv.prototype._transform = function (
  this: Decipheriv,
  chunk: string | Buffer | ArrayBufferView,
  encoding: string,
  callback: TransformCallback
): void {
  if (typeof chunk === 'string') {
    chunk = Buffer.from(chunk, encoding);
  }
  this.push(Buffer.from(this[kHandle].update(chunk)));
  callback();
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Decipheriv.prototype._flush = function (
  this: Decipheriv,
  callback: TransformCallback
): void {
  this.push(Buffer.from(this[kHandle].final()));
  callback();
};

export function createCipheriv(
  algorithm: string,
  key: string | ArrayBuffer | ArrayBufferView | KeyObject | CryptoKey,
  iv: string | ArrayBuffer | ArrayBufferView | null,
  options: TransformOptions = {}
): Cipheriv {
  return new Cipheriv(algorithm, key, iv, options);
}

export function createDecipheriv(
  algorithm: string,
  key: string | ArrayBuffer | ArrayBufferView | KeyObject | CryptoKey,
  iv: string | ArrayBuffer | ArrayBufferView | null,
  options: TransformOptions = {}
): Decipheriv {
  return new Decipheriv(algorithm, key, iv, options);
}

// ======================================================================================

interface HashOptions {
  padding?: number;
  oaepHash?: string;
  oaepLabel?: string | ArrayBuffer | ArrayBufferView;
  encoding?: string;
}

export const kRsaPkcs1Padding = 1;
export const kRsaNoPadding = 3;
export const kRsaPkcs1OaepPadding = 4;

function getPaddingAndHash(options: HashOptions): PublicPrivateCipherOptions {
  const {
    padding = kRsaPkcs1Padding,
    oaepHash = 'sha256',
    oaepLabel,
    encoding = 'utf8',
  } = options;

  validateNumber(padding, 'options.padding');
  validateString(oaepHash, 'options.oaepHash');

  let label: ArrayBufferView | ArrayBuffer | undefined;
  if (oaepLabel !== undefined) {
    if (typeof oaepLabel === 'string') {
      label = Buffer.from(oaepLabel, encoding);
    } else if (isAnyArrayBuffer(oaepLabel) || isArrayBufferView(oaepLabel)) {
      label = oaepLabel;
    } else {
      throw new ERR_INVALID_ARG_TYPE(
        'options.oaepLabel',
        ['string', 'Buffer', 'TypedArray', 'DataView', 'ArrayBuffer'],
        oaepLabel
      );
    }
  }

  return { padding, oaepHash, oaepLabel: label };
}

export function privateEncrypt(
  privateKey: CreateAsymmetricKeyOptions | KeyData,
  buffer: string | ArrayBufferView | ArrayBuffer
): Buffer {
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (privateKey === undefined) {
    throw new ERR_MISSING_ARGS('privateKey');
  }
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (buffer === undefined) {
    throw new ERR_MISSING_ARGS('buffer');
  }
  const key = ((): CryptoKey => {
    if (privateKey instanceof CryptoKey) {
      if (privateKey.type !== 'private') {
        throw new ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE(
          privateKey.type,
          'private'
        );
      }
      return privateKey;
    } else if (isKeyObject(privateKey)) {
      if (privateKey.type !== 'private') {
        throw new ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE(
          privateKey.type,
          'private'
        );
      }
      return getKeyObjectHandle(privateKey);
    } else {
      const pvtKey = createPrivateKey(privateKey as CreateAsymmetricKeyOptions);
      return getKeyObjectHandle(pvtKey);
    }
  })();
  if (typeof buffer === 'string') {
    const { encoding = 'utf8' } = privateKey as { encoding?: string };
    buffer = Buffer.from(buffer, encoding);
  }

  return Buffer.from(
    cryptoImpl.privateEncrypt(
      key,
      buffer,
      getPaddingAndHash(privateKey as HashOptions)
    )
  );
}

export function privateDecrypt(
  privateKey: CreateAsymmetricKeyOptions | KeyData,
  buffer: string | ArrayBufferView | ArrayBuffer
): Buffer {
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (privateKey === undefined) {
    throw new ERR_MISSING_ARGS('privateKey');
  }
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (buffer === undefined) {
    throw new ERR_MISSING_ARGS('buffer');
  }
  const key = ((): CryptoKey => {
    if (privateKey instanceof CryptoKey) {
      if (privateKey.type !== 'private') {
        throw new ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE(
          privateKey.type,
          'private'
        );
      }
      return privateKey;
    } else if (isKeyObject(privateKey)) {
      if (privateKey.type !== 'private') {
        throw new ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE(
          privateKey.type,
          'private'
        );
      }
      return getKeyObjectHandle(privateKey);
    } else {
      const pvtKey = createPrivateKey(privateKey as CreateAsymmetricKeyOptions);
      return getKeyObjectHandle(pvtKey);
    }
  })();
  if (typeof buffer === 'string') {
    const { encoding = 'utf8' } = privateKey as { encoding?: string };
    buffer = Buffer.from(buffer, encoding);
  }

  return Buffer.from(
    cryptoImpl.privateDecrypt(
      key,
      buffer,
      getPaddingAndHash(privateKey as HashOptions)
    )
  );
}

export function publicEncrypt(
  key: CreateAsymmetricKeyOptions | KeyData,
  buffer: string | ArrayBufferView | ArrayBuffer
): Buffer {
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (key === undefined) {
    throw new ERR_MISSING_ARGS('privateKey');
  }
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (buffer === undefined) {
    throw new ERR_MISSING_ARGS('buffer');
  }
  const pkey = ((): CryptoKey => {
    if (key instanceof CryptoKey) {
      if (key.type !== 'public') {
        throw new ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE(key.type, 'public');
      }
      return key;
    } else if (isKeyObject(key)) {
      if (key.type !== 'public') {
        throw new ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE(key.type, 'public');
      }
      return getKeyObjectHandle(key);
    } else {
      const pubKey = createPublicKey(key as CreateAsymmetricKeyOptions);
      return getKeyObjectHandle(pubKey);
    }
  })();
  if (typeof buffer === 'string') {
    const { encoding = 'utf8' } = key as { encoding?: string };
    buffer = Buffer.from(buffer, encoding);
  }

  return Buffer.from(
    cryptoImpl.publicEncrypt(
      pkey,
      buffer,
      getPaddingAndHash(key as HashOptions)
    )
  );
}

export function publicDecrypt(
  key: CreateAsymmetricKeyOptions | KeyData,
  buffer: string | ArrayBufferView | ArrayBuffer
): Buffer {
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (key === undefined) {
    throw new ERR_MISSING_ARGS('privateKey');
  }
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (buffer === undefined) {
    throw new ERR_MISSING_ARGS('buffer');
  }
  const pkey = ((): CryptoKey => {
    if (key instanceof CryptoKey) {
      if (key.type !== 'public') {
        throw new ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE(key.type, 'public');
      }
      return key;
    } else if (isKeyObject(key)) {
      if (key.type !== 'public') {
        throw new ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE(key.type, 'public');
      }
      return getKeyObjectHandle(key);
    } else {
      const pubKey = createPublicKey(key as CreateAsymmetricKeyOptions);
      return getKeyObjectHandle(pubKey);
    }
  })();
  if (typeof buffer === 'string') {
    const { encoding = 'utf8' } = key as { encoding?: string };
    buffer = Buffer.from(buffer, encoding);
  }

  return Buffer.from(
    cryptoImpl.publicDecrypt(
      pkey,
      buffer,
      getPaddingAndHash(key as HashOptions)
    )
  );
}
