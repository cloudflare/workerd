// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Pins what JavaScript sees of CryptoKey: the class, the shape and values of its instances from
// every place that makes them (SubtleCrypto, node:crypto, X509Certificate, a CRYPTO_KEY binding),
// and how keys pass back into SubtleCrypto and node:crypto. The lines it records must match
// crypto-key-parity-expected.js exactly, at the oldest date and with every flag. Errors in them
// carry this file's line numbers, so moving code here means regenerating the expectations from
// the C++ implementation's output.

import { strictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';
import * as nodeCrypto from 'node:crypto';
import { inspect } from 'node:util';
import { oldest, newest } from 'crypto-key-parity-expected.js';

const subtle = crypto.subtle;

const cert = `-----BEGIN CERTIFICATE-----
MIID6DCCAtCgAwIBAgIUFH02wcL3Qgben6tfIibXitsApCYwDQYJKoZIhvcNAQEL
BQAwejELMAkGA1UEBhMCVVMxCzAJBgNVBAgMAkNBMQswCQYDVQQHDAJTRjEPMA0G
A1UECgwGSm95ZW50MRAwDgYDVQQLDAdOb2RlLmpzMQwwCgYDVQQDDANjYTExIDAe
BgkqhkiG9w0BCQEWEXJ5QHRpbnljbG91ZHMub3JnMCAXDTIyMDkwMzIxNDAzN1oY
DzIyOTYwNjE3MjE0MDM3WjB9MQswCQYDVQQGEwJVUzELMAkGA1UECAwCQ0ExCzAJ
BgNVBAcMAlNGMQ8wDQYDVQQKDAZKb3llbnQxEDAOBgNVBAsMB05vZGUuanMxDzAN
BgNVBAMMBmFnZW50MTEgMB4GCSqGSIb3DQEJARYRcnlAdGlueWNsb3Vkcy5vcmcw
ggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDUVjIK+yDTgnCT3CxChO0E
37q9VuHdrlKeKLeQzUJW2yczSfNzX/0zfHpjY+zKWie39z3HCJqWxtiG2wxiOI8c
3WqWOvzVmdWADlh6EfkIlg+E7VC6JaKDA+zabmhPvnuu3JzogBMnsWl68lCXzuPx
deQAmEwNtqjrh74DtM+Ud0ulb//Ixjxo1q3rYKu+aaexSramuee6qJta2rjrB4l8
B/bU+j1mDf9XQQfSjo9jRnp4hiTFdBl2k+lZzqE2L/rhu6EMjA2IhAq/7xA2MbLo
9cObVUin6lfoo5+JKRgT9Fp2xEgDOit+2EA/S6oUfPNeLSVUqmXOSWlXlwlb9Nxr
AgMBAAGjYTBfMF0GCCsGAQUFBwEBBFEwTzAjBggrBgEFBQcwAYYXaHR0cDovL29j
c3Aubm9kZWpzLm9yZy8wKAYIKwYBBQUHMAKGHGh0dHA6Ly9jYS5ub2RlanMub3Jn
L2NhLmNlcnQwDQYJKoZIhvcNAQELBQADggEBAMM0mBBjLMt9pYXePtUeNO0VTw9y
FWCM8nAcAO2kRNwkJwcsispNpkcsHZ5o8Xf5mpCotdvziEWG1hyxwU6nAWyNOLcN
G0a0KUfbMO3B6ZYe1GwPDjXaQnv75SkAdxgX5zOzca3xnhITcjUUGjQ0fbDfwFV5
ix8mnzvfXjDONdEznVa7PFcN6QliFUMwR/h8pCRHtE5+a10OSPeJSrGG+FtrGnRW
G1IJUv6oiGF/MvWCr84REVgc1j78xomGANJIu2hN7bnD1nEMON6em8IfnDOUtynV
9wfWTqiQYD5Zifj6WcGa0aAHMuetyFG4lIfMAHmd3gaKpks7j9l26LwRPvI=
-----END CERTIFICATE-----
`;

const ca = `-----BEGIN CERTIFICATE-----
MIIDlDCCAnygAwIBAgIUSrFsjf1qfQ0t/KvfnEsOksatAikwDQYJKoZIhvcNAQEL
BQAwejELMAkGA1UEBhMCVVMxCzAJBgNVBAgMAkNBMQswCQYDVQQHDAJTRjEPMA0G
A1UECgwGSm95ZW50MRAwDgYDVQQLDAdOb2RlLmpzMQwwCgYDVQQDDANjYTExIDAe
BgkqhkiG9w0BCQEWEXJ5QHRpbnljbG91ZHMub3JnMCAXDTIyMDkwMzIxNDAzN1oY
DzIyOTYwNjE3MjE0MDM3WjB6MQswCQYDVQQGEwJVUzELMAkGA1UECAwCQ0ExCzAJ
BgNVBAcMAlNGMQ8wDQYDVQQKDAZKb3llbnQxEDAOBgNVBAsMB05vZGUuanMxDDAK
BgNVBAMMA2NhMTEgMB4GCSqGSIb3DQEJARYRcnlAdGlueWNsb3Vkcy5vcmcwggEi
MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDNvf4OGGep+ak+4DNjbuNgy0S/
AZPxahEFp4gpbcvsi9YLOPZ31qpilQeQf7d27scIZ02Qx1YBAzljxELB8H/ZxuYS
cQK0s+DNP22xhmgwMWznO7TezkHP5ujN2UkbfbUpfUxGFgncXeZf9wR7yFWppeHi
RWNBOgsvY7sTrS12kXjWGjqntF7xcEDHc7h+KyF6ZjVJZJCnP6pJEQ+rUjd51eCZ
Xt4WjowLnQiCS1VKzXiP83a++Ma1BKKkUitTR112/Uwd5eGoiByhmLzb/BhxnHJN
07GXjhlMItZRm/jfbZsx1mwnNOO3tx4r08l+DaqkinIadvazs+1ugCaKQn8xAgMB
AAGjEDAOMAwGA1UdEwQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBAFqG0RXURDam
56x5accdg9sY5zEGP5VQhkK3ZDc2NyNNa25rwvrjCpO+e0OSwKAmm4aX6iIf2woY
wF2f9swWYzxn9CG4fDlUA8itwlnHxupeL4fGMTYb72vf31plUXyBySRsTwHwBloc
F7KvAZpYYKN9EMH1S/267By6H2I33BT/Ethv//n8dSfmuCurR1kYRaiOC4PVeyFk
B3sj8TtolrN0y/nToWUhmKiaVFnDx3odQ00yhmxR3t21iB7yDkko6D8Vf2dVC4j/
YYBVprXGlTP/hiYRLDoP20xKOYznx5cvHPJ9p+lVcOZUJsJj/Iy750+2n5UiBmXt
lz88C25ucKA=
-----END CERTIFICATE-----
`;

const certKey = `-----BEGIN RSA PRIVATE KEY-----
MIIEpAIBAAKCAQEA1FYyCvsg04Jwk9wsQoTtBN+6vVbh3a5Snii3kM1CVtsnM0nz
c1/9M3x6Y2Psylont/c9xwialsbYhtsMYjiPHN1qljr81ZnVgA5YehH5CJYPhO1Q
uiWigwPs2m5oT757rtyc6IATJ7FpevJQl87j8XXkAJhMDbao64e+A7TPlHdLpW//
yMY8aNat62CrvmmnsUq2prnnuqibWtq46weJfAf21Po9Zg3/V0EH0o6PY0Z6eIYk
xXQZdpPpWc6hNi/64buhDIwNiIQKv+8QNjGy6PXDm1VIp+pX6KOfiSkYE/RadsRI
AzorfthAP0uqFHzzXi0lVKplzklpV5cJW/TcawIDAQABAoIBAAvbtHfAhpjJVBgt
15rvaX04MWmZjIugzKRgib/gdq/7FTlcC+iJl85kSUF7tyGl30n62MxgwqFhAX6m
hQ6HMhbelrFFIhGbwbyhEHfgwROlrcAysKt0pprCgVvBhrnNXYLqdyjU3jz9P3LK
TY3s0/YMK2uNFdI+PTjKH+Z9Foqn9NZUnUonEDepGyuRO7fLeccWJPv2L4CR4a/5
ku4VbDgVpvVSVRG3PSVzbmxobnpdpl52og+T7tPx1cLnIknPtVljXPWtZdfekh2E
eAp2KxCCHOKzzG3ItBKsVu0woeqEpy8JcoO6LbgmEoVnZpgmtQClbBgef8+i+oGE
BgW9nmECgYEA8gA63QQuZOUC56N1QXURexN2PogF4wChPaCTFbQSJXvSBkQmbqfL
qRSD8P0t7GOioPrQK6pDwFf4BJB01AvkDf8Z6DxxOJ7cqIC7LOwDupXocWX7Q0Qk
O6cwclBVsrDZK00v60uRRpl/a39GW2dx7IiQDkKQndLh3/0TbMIWHNcCgYEA4J6r
yinZbLpKw2+ezhi4B4GT1bMLoKboJwpZVyNZZCzYR6ZHv+lS7HR/02rcYMZGoYbf
n7OHwF4SrnUS7vPhG4g2ZsOhKQnMvFSQqpGmK1ZTuoKGAevyvtouhK/DgtLWzGvX
9fSahiq/UvfXs/z4M11q9Rv9ztPCmG1cwSEHlo0CgYEAogQNZJK8DMhVnYcNpXke
7uskqtCeQE/Xo06xqkIYNAgloBRYNpUYAGa/vsOBz1UVN/kzDUi8ezVp0oRz8tLT
J5u2WIi+tE2HJTiqF3UbOfvK1sCT64DfUSCpip7GAQ/tFNRkVH8PD9kMOYfILsGe
v+DdsO5Xq5HXrwHb02BNNZkCgYBsl8lt33WiPx5OBfS8pu6xkk+qjPkeHhM2bKZs
nkZlS9j0KsudWGwirN/vkkYg8zrKdK5AQ0dqFRDrDuasZ3N5IA1M+V88u+QjWK7o
B6pSYVXxYZDv9OZSpqC+vUrEQLJf+fNakXrzSk9dCT1bYv2Lt6ox/epix7XYg2bI
Z/OHMQKBgQC2FUGhlndGeugTJaoJ8nhT/0VfRUX/h6sCgSerk5qFr/hNCBV4T022
x0NDR2yLG6MXyqApJpG6rh3QIDElQoQCNlI3/KJ6JfEfmqrLLN2OigTvA5sE4fGU
Dp/ha8OQAx95EwXuaG7LgARduvOIK3x8qi8KsZoUGJcg2ywurUbkWA==
-----END RSA PRIVATE KEY-----
`;

function hex(buffer) {
  return Buffer.from(buffer).toString('hex');
}

function errorText(e) {
  return `${e?.name}: ${e?.message}`;
}

// Calls fn and describes how it completes, awaiting a promise.
async function settle(fn) {
  let result;
  try {
    result = fn();
  } catch (e) {
    return `THROWS ${errorText(e)}`;
  }
  if (!(result instanceof Promise)) return `RETURNS ${describe(result)}`;
  try {
    return `resolves ${describe(await result)}`;
  } catch (e) {
    return `rejects ${errorText(e)}`;
  }
}

function describe(value) {
  if (value === undefined) return 'undefined';
  if (value === null) return 'null';
  if (typeof value !== 'object' && typeof value !== 'function') {
    return JSON.stringify(value);
  }
  if (value instanceof CryptoKey) {
    return `CryptoKey(${value.type},${value.algorithm.name},${value.extractable},${value.usages})`;
  }
  if (value instanceof ArrayBuffer) return `ArrayBuffer(${value.byteLength})`;
  if (ArrayBuffer.isView(value)) {
    return `${value.constructor.name}(${value.byteLength})`;
  }
  if (Array.isArray(value)) return `[${value.map(describe).join(',')}]`;
  return `{${Object.keys(value).join(',')}}`;
}

function descriptor(object, key) {
  const d = Object.getOwnPropertyDescriptor(object, key);
  if (d === undefined) return 'none';
  return Object.entries(d)
    .map(([name, v]) => {
      if (typeof v === 'function') return `${name}=function ${v.name}`;
      if (typeof v === 'object' && v !== null) {
        return `${name}=${Object.prototype.toString.call(v)}`;
      }
      return `${name}=${String(v)}`;
    })
    .join(' ');
}

function algorithmText(algorithm) {
  return JSON.stringify(algorithm, (k, v) => {
    if (v instanceof ArrayBuffer) return `ArrayBuffer(${hex(v)})`;
    if (ArrayBuffer.isView(v)) return `${v.constructor.name}(${hex(v)})`;
    return v;
  });
}

// Everything observable of one instance.
function describeKey(lines, label, key) {
  lines.push(`${label}: ${describe(key)}`);
  lines.push(`${label} instanceof ${key instanceof CryptoKey}`);
  lines.push(
    `${label} [[Prototype]] is CryptoKey.prototype ${
      Object.getPrototypeOf(key) === CryptoKey.prototype
    }`
  );
  lines.push(`${label} own ${Reflect.ownKeys(key).map(String).join(',')}`);
  for (const name of Reflect.ownKeys(key)) {
    lines.push(`${label} own ${String(name)} ${descriptor(key, name)}`);
  }
  lines.push(`${label} keys ${Object.keys(key).join(',')}`);
  lines.push(`${label} algorithm ${algorithmText(key.algorithm)}`);
  lines.push(
    `${label} algorithm [[Prototype]] is Object.prototype ${
      Object.getPrototypeOf(key.algorithm) === Object.prototype
    }`
  );
  lines.push(
    `${label} same algorithm ${key.algorithm === key.algorithm} same usages ${
      key.usages === key.usages
    }`
  );
  lines.push(
    `${label} usages isArray ${Array.isArray(key.usages)} frozen ${Object.isFrozen(
      key.usages
    )}`
  );
  lines.push(`${label} toString ${Object.prototype.toString.call(key)}`);
  lines.push(
    `${label} JSON ${JSON.stringify(key, (k, v) => (ArrayBuffer.isView(v) || v instanceof ArrayBuffer ? 'bytes' : v))}`
  );
  lines.push(`${label} inspect ${inspect(key, { breakLength: Infinity })}`);
  let cloned;
  try {
    structuredClone(key);
    cloned = 'cloned';
  } catch (e) {
    cloned = errorText(e);
  }
  lines.push(`${label} structuredClone ${cloned}`);
}

async function run(env) {
  const lines = [];

  // The class.
  const proto = CryptoKey.prototype;
  lines.push(`proto own ${Reflect.ownKeys(proto).map(String).join(',')}`);
  for (const name of Reflect.ownKeys(proto)) {
    lines.push(`proto ${String(name)} ${descriptor(proto, name)}`);
  }
  lines.push(
    `proto [[Prototype]] is Object.prototype ${
      Object.getPrototypeOf(proto) === Object.prototype
    }`
  );
  lines.push(`ctor own ${Reflect.ownKeys(CryptoKey).map(String).join(',')}`);
  for (const name of Reflect.ownKeys(CryptoKey)) {
    lines.push(`ctor ${String(name)} ${descriptor(CryptoKey, name)}`);
  }
  lines.push(
    `ctor [[Prototype]] is Function.prototype ${
      Object.getPrototypeOf(CryptoKey) === Function.prototype
    }`
  );
  lines.push(
    `global prototype CryptoKey ${descriptor(Object.getPrototypeOf(globalThis), 'CryptoKey')}`
  );
  lines.push(`globalThis CryptoKey ${descriptor(globalThis, 'CryptoKey')}`);
  for (const [label, fn] of [
    ['new', () => new CryptoKey()],
    ['call', () => CryptoKey()],
  ]) {
    try {
      fn();
      lines.push(`${label} CryptoKey returns`);
    } catch (e) {
      // Not the stack, which under TS_WEBCRYPTO begins in the bootstrap (crypto/AGENTS.md).
      lines.push(`${label} CryptoKey ${errorText(e)}`);
    }
  }

  // Keys from every place that makes them.
  const hmac = await subtle.importKey(
    'raw',
    new Uint8Array(32).fill(7),
    { name: 'HMAC', hash: 'SHA-256' },
    true,
    ['verify', 'sign']
  );
  describeKey(lines, 'hmac', hmac);
  const aes = await subtle.importKey(
    'raw',
    new Uint8Array(16).fill(1),
    'AES-GCM',
    false,
    ['wrapKey', 'encrypt', 'unwrapKey', 'decrypt']
  );
  describeKey(lines, 'aes', aes);
  const aesKw = await subtle.generateKey(
    { name: 'AES-KW', length: 128 },
    true,
    ['wrapKey', 'unwrapKey']
  );
  describeKey(lines, 'aes-kw generated', aesKw);
  const rsaPem = nodeCrypto.createPrivateKey(certKey);
  const rsaPkcs8 = rsaPem.export({ type: 'pkcs8', format: 'der' });
  const rsa = await subtle.importKey(
    'pkcs8',
    rsaPkcs8,
    { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' },
    true,
    ['sign']
  );
  describeKey(lines, 'rsa private', rsa);
  const rsaPair = await subtle.generateKey(
    {
      name: 'RSA-OAEP',
      modulusLength: 1024,
      publicExponent: new Uint8Array([1, 0, 1]),
      hash: 'SHA-1',
    },
    true,
    ['encrypt', 'decrypt']
  );
  describeKey(lines, 'rsa-oaep public', rsaPair.publicKey);
  describeKey(lines, 'rsa-oaep private', rsaPair.privateKey);
  lines.push(
    `rsa-oaep pair keys ${Object.keys(rsaPair).join(',')} proto ${
      Object.getPrototypeOf(rsaPair) === Object.prototype
    }`
  );
  const ecPair = await subtle.generateKey(
    { name: 'ECDH', namedCurve: 'P-256' },
    true,
    ['deriveBits', 'deriveKey']
  );
  describeKey(lines, 'ecdh public', ecPair.publicKey);
  describeKey(lines, 'ecdh private', ecPair.privateKey);
  const edPair = await subtle.generateKey('Ed25519', true, ['sign', 'verify']);
  describeKey(lines, 'ed25519 public', edPair.publicKey);
  const xPair = await subtle.generateKey('X25519', true, ['deriveBits']);
  describeKey(lines, 'x25519 private', xPair.privateKey);
  const derived = await subtle.deriveKey(
    { name: 'ECDH', public: ecPair.publicKey },
    ecPair.privateKey,
    { name: 'AES-CBC', length: 256 },
    true,
    ['encrypt']
  );
  describeKey(lines, 'derived', derived);

  describeKey(lines, 'binding', env.key);

  const secretObject = nodeCrypto.createSecretKey(new Uint8Array(16).fill(3));
  const fromNode = secretObject.toCryptoKey('AES-CTR', true, ['encrypt']);
  describeKey(lines, 'toCryptoKey', fromNode);
  const x509 = new nodeCrypto.X509Certificate(cert);
  const x509Key = x509.publicKey.toCryptoKey(
    { name: 'RSA-PSS', hash: 'SHA-384' },
    true,
    ['verify']
  );
  describeKey(lines, 'x509 toCryptoKey', x509Key);

  // Keys passed back in.
  const data = new Uint8Array([1, 2, 3]);
  const mac = await subtle.sign('HMAC', hmac, data);
  lines.push(`hmac sign ${hex(mac)}`);
  lines.push(`hmac verify ${await subtle.verify('HMAC', hmac, mac, data)}`);
  lines.push(`hmac export ${hex(await subtle.exportKey('raw', hmac))}`);
  const iv = new Uint8Array(12);
  lines.push(
    `aes encrypt ${hex(await subtle.encrypt({ name: 'AES-GCM', iv }, aes, data))}`
  );
  const wrapped = await subtle.wrapKey('raw', hmac, aesKw, 'AES-KW');
  const unwrapped = await subtle.unwrapKey(
    'raw',
    wrapped,
    aesKw,
    'AES-KW',
    { name: 'HMAC', hash: 'SHA-256' },
    true,
    ['sign']
  );
  lines.push(
    `unwrapped export ${hex(await subtle.exportKey('raw', unwrapped))}`
  );
  const bits = await subtle.deriveBits(
    { name: 'X25519', public: xPair.publicKey },
    xPair.privateKey,
    256
  );
  lines.push(`x25519 deriveBits ${bits.byteLength}`);
  lines.push(`binding sign ${hex(await subtle.sign('HMAC', env.key, data))}`);
  lines.push(`fromNode export ${hex(await subtle.exportKey('raw', fromNode))}`);

  // node:crypto over keys of either origin. Each line is a call that may throw.
  const node = async (label, fn) => lines.push(`${label} ${await settle(fn)}`);
  const keyObject = nodeCrypto.KeyObject.from(hmac);
  await node('KeyObject.from', () =>
    [
      keyObject.type,
      keyObject.symmetricKeySize,
      keyObject.export().toString('hex'),
    ].join(' ')
  );
  await node('KeyObject.from equals', () =>
    keyObject.equals(nodeCrypto.KeyObject.from(unwrapped))
  );
  await node('createHmac with CryptoKey', () =>
    nodeCrypto.createHmac('sha256', hmac).update(data).digest('hex')
  );
  await node('createHmac with KeyObject', () =>
    nodeCrypto.createHmac('sha256', keyObject).update(data).digest('hex')
  );
  await node('createHmac with KeyObject of node key', () =>
    nodeCrypto.createHmac('sha256', secretObject).update(data).digest('hex')
  );
  const rsaObject = nodeCrypto.KeyObject.from(rsa);
  await node('rsa KeyObject', () =>
    [
      rsaObject.type,
      rsaObject.asymmetricKeyType,
      JSON.stringify(rsaObject.asymmetricKeyDetails, (k, v) =>
        typeof v === 'bigint' ? `${v}n` : v
      ),
    ].join(' ')
  );
  await node('rsa KeyObject export', () =>
    rsaObject
      .export({ type: 'pkcs8', format: 'der' })
      .toString('hex')
      .slice(0, 32)
  );
  await node(
    'createPublicKey(CryptoKey)',
    () => nodeCrypto.createPublicKey(rsa).type
  );
  await node(
    'createPublicKey(KeyObject of node key)',
    () => nodeCrypto.createPublicKey(rsaPem).asymmetricKeyType
  );
  await node(
    'createPrivateKey(CryptoKey)',
    () => nodeCrypto.createPrivateKey(rsa).type
  );
  const signature = nodeCrypto.sign('sha256', data, rsaPem);
  await node('sign(CryptoKey)', () =>
    nodeCrypto.sign('sha256', data, rsa).toString('hex').slice(0, 32)
  );
  await node('verify(x509 KeyObject)', () =>
    nodeCrypto.verify('sha256', data, x509.publicKey, signature)
  );
  await node('verify(x509 toCryptoKey)', () =>
    nodeCrypto.verify('sha256', data, x509Key, signature)
  );
  await node('x509 checkPrivateKey', () =>
    x509.checkPrivateKey(nodeCrypto.KeyObject.from(rsa))
  );
  await node('x509 checkPrivateKey(node key)', () =>
    x509.checkPrivateKey(rsaPem)
  );
  await node('x509 verify', () =>
    x509.verify(new nodeCrypto.X509Certificate(ca).publicKey)
  );
  await node(
    'x509 KeyObject.from(toCryptoKey)',
    () => nodeCrypto.KeyObject.from(x509Key).asymmetricKeyType
  );
  await node('createCipheriv', () =>
    hex(
      nodeCrypto
        .createCipheriv(
          'aes-128-ctr',
          nodeCrypto.KeyObject.from(fromNode),
          new Uint8Array(16)
        )
        .update(data)
    )
  );
  await node('generateKeyPairSync', () => {
    const { publicKey } = nodeCrypto.generateKeyPairSync('ed25519');
    const key = publicKey.toCryptoKey('Ed25519', true, ['verify']);
    return `${publicKey.type} ${publicKey.asymmetricKeyType} ${describe(key)}`;
  });
  await node(
    'KeyObject.from(CRYPTO_KEY binding)',
    () => nodeCrypto.KeyObject.from(env.key).symmetricKeySize
  );

  // Values that are not CryptoKeys.
  const fakes = [
    ['Object.create(CryptoKey.prototype)', Object.create(CryptoKey.prototype)],
    ['Object.create(key)', Object.create(hmac)],
    ['proxy of key', new Proxy(hmac, {})],
    ['plain object', { type: 'secret' }],
  ];
  for (const [label, fake] of fakes) {
    lines.push(
      `${label} instanceof ${fake instanceof CryptoKey} type ${fake.type}`
    );
    lines.push(
      `${label} sign ${await settle(() => subtle.sign('HMAC', fake, data))}`
    );
    lines.push(
      `${label} deriveBits public ${await settle(() =>
        subtle.deriveBits(
          { name: 'X25519', public: fake },
          xPair.privateKey,
          256
        )
      )}`
    );
    lines.push(
      `${label} KeyObject.from ${await settle(() => nodeCrypto.KeyObject.from(fake).type)}`
    );
    lines.push(
      `${label} createHmac ${await settle(() => nodeCrypto.createHmac('sha256', fake).digest('hex'))}`
    );
  }

  // Changes user code can make to an instance.
  const changed = await subtle.importKey(
    'raw',
    new Uint8Array(32).fill(9),
    { name: 'HMAC', hash: 'SHA-256' },
    true,
    ['sign']
  );
  lines.push(
    `assign type ${await settle(() => (changed.type = 'public'))} -> ${changed.type}`
  );
  changed.algorithm.name = 'changed';
  changed.usages.push('verify');
  lines.push(
    `mutated algorithm ${changed.algorithm.name} usages ${changed.usages}`
  );
  lines.push(
    `mutated still signs ${hex(await subtle.sign('HMAC', changed, data)).slice(0, 16)}`
  );
  lines.push(`delete type ${delete changed.type} -> ${changed.type}`);
  lines.push(
    `deleted still signs ${hex(await subtle.sign('HMAC', changed, data)).slice(0, 16)}`
  );
  Object.defineProperty(changed, 'extractable', { value: false });
  lines.push(`redefined extractable ${changed.extractable}`);
  lines.push(
    `redefined export ${await settle(() => subtle.exportKey('raw', changed))}`
  );
  Object.freeze(changed);
  lines.push(
    `frozen signs ${hex(await subtle.sign('HMAC', changed, data)).slice(0, 16)} KeyObject ${await settle(() => nodeCrypto.KeyObject.from(changed)?.type)}`
  );
  lines.push(
    `setPrototypeOf frozen ${await settle(() => Object.setPrototypeOf(changed, null))}`
  );
  // A key given another prototype stays a key: instanceof follows the new prototype, and the key
  // still signs.
  const unfrozen = await subtle.importKey(
    'raw',
    new Uint8Array(32),
    { name: 'HMAC', hash: 'SHA-256' },
    false,
    ['sign']
  );
  Object.setPrototypeOf(unfrozen, null);
  lines.push(
    `null prototype instanceof ${unfrozen instanceof CryptoKey} signs ${await settle(
      () => subtle.sign('HMAC', unfrozen, data)
    )}`
  );

  // Assigning CryptoKey.prototype, which is writable unless immutable_api_prototypes, and the
  // prototype of keys made afterwards.
  {
    const original = CryptoKey.prototype;
    const replacement = Object.create(original);
    let assigned;
    try {
      CryptoKey.prototype = replacement;
      assigned = CryptoKey.prototype === replacement;
    } catch (e) {
      assigned = `throws ${e.constructor.name}`;
    }
    const later = await subtle.importKey(
      'raw',
      new Uint8Array(32),
      { name: 'HMAC', hash: 'SHA-256' },
      false,
      ['sign']
    );
    lines.push(
      `assigned prototype ${assigned} later key has it ${
        Object.getPrototypeOf(later) === replacement
      } instanceof ${later instanceof CryptoKey}`
    );
    if (assigned === true) CryptoKey.prototype = original;
  }

  return lines;
}

function compare(actual, expectedLines, variant) {
  // On a mismatch, print everything, so the expectations can be regenerated from the C++
  // implementation's output.
  if (actual.join('\n') !== expectedLines.join('\n')) {
    console.log(`ACTUAL BEGIN ${variant}\n${actual.join('\n')}\nACTUAL END`);
  }
  for (let i = 0; i < Math.max(actual.length, expectedLines.length); i++) {
    strictEqual(actual[i], expectedLines[i], `line ${i + 1}`);
  }
}

export default {
  async test(ctrl, env) {
    // As in crypto-subtle-parity-test.js: the era is told apart by capture_async_api_throws, and
    // each service is the `classic` or `spec` variant.
    const era = Cloudflare.compatibilityFlags['capture_async_api_throws']
      ? newest
      : oldest;
    compare(await run(env), era[env.variant], env.variant);
  },
};
