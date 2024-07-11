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
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT ORs
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */

import {
  deepStrictEqual,
  ok,
  strictEqual,
  throws,
} from 'node:assert';

import { Buffer } from 'node:buffer';

import {
  X509Certificate,
  PublicKeyObject,
} from 'node:crypto';

const cert = Buffer.from(`-----BEGIN CERTIFICATE-----
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
`);

const ca = Buffer.from(`-----BEGIN CERTIFICATE-----
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
`);

const der = Buffer.from(
  '308203e8308202d0a0030201020214147d36c1c2f74206de9fab5f2226d78adb00a42630' +
  '0d06092a864886f70d01010b0500307a310b3009060355040613025553310b3009060355' +
  '04080c024341310b300906035504070c025346310f300d060355040a0c064a6f79656e74' +
  '3110300e060355040b0c074e6f64652e6a73310c300a06035504030c036361313120301e' +
  '06092a864886f70d010901161172794074696e79636c6f7564732e6f72673020170d3232' +
  '303930333231343033375a180f32323936303631373231343033375a307d310b30090603' +
  '55040613025553310b300906035504080c024341310b300906035504070c025346310f30' +
  '0d060355040a0c064a6f79656e743110300e060355040b0c074e6f64652e6a73310f300d' +
  '06035504030c066167656e74313120301e06092a864886f70d010901161172794074696e' +
  '79636c6f7564732e6f726730820122300d06092a864886f70d01010105000382010f0030' +
  '82010a0282010100d456320afb20d3827093dc2c4284ed04dfbabd56e1ddae529e28b790' +
  'cd4256db273349f3735ffd337c7a6363ecca5a27b7f73dc7089a96c6d886db0c62388f1c' +
  'dd6a963afcd599d5800e587a11f908960f84ed50ba25a28303ecda6e684fbe7baedc9ce8' +
  '801327b1697af25097cee3f175e400984c0db6a8eb87be03b4cf94774ba56fffc8c63c68' +
  'd6adeb60abbe69a7b14ab6a6b9e7baa89b5adab8eb07897c07f6d4fa3d660dff574107d2' +
  '8e8f63467a788624c574197693e959cea1362ffae1bba10c8c0d88840abfef103631b2e8' +
  'f5c39b5548a7ea57e8a39f89291813f45a76c448033a2b7ed8403f4baa147cf35e2d2554' +
  'aa65ce49695797095bf4dc6b0203010001a361305f305d06082b06010505070101045130' +
  '4f302306082b060105050730018617687474703a2f2f6f6373702e6e6f64656a732e6f72' +
  '672f302806082b06010505073002861c687474703a2f2f63612e6e6f64656a732e6f7267' +
  '2f63612e63657274300d06092a864886f70d01010b05000382010100c3349810632ccb7d' +
  'a585de3ed51e34ed154f0f7215608cf2701c00eda444dc2427072c8aca4da6472c1d9e68' +
  'f177f99a90a8b5dbf3884586d61cb1c14ea7016c8d38b70d1b46b42947db30edc1e9961e' +
  'd46c0f0e35da427bfbe52900771817e733b371adf19e12137235141a34347db0dfc05579' +
  '8b1f269f3bdf5e30ce35d1339d56bb3c570de9096215433047f87ca42447b44e7e6b5d0e' +
  '48f7894ab186f85b6b1a74561b520952fea888617f32f582afce1111581cd63efcc68986' +
  '00d248bb684dedb9c3d6710c38de9e9bc21f9c3394b729d5f707d64ea890603e5989f8fa' +
  '59c19ad1a00732e7adc851b89487cc00799dde068aa64b3b8fd976e8bc113ef2',
  'hex');

const key = Buffer.from(`-----BEGIN RSA PRIVATE KEY-----
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
`);

const subjectCheck = `C=US
ST=CA
L=SF
O=Joyent
OU=Node.js
CN=agent1
emailAddress=ry@tinyclouds.org`;

const issuerCheck = `C=US
ST=CA
L=SF
O=Joyent
OU=Node.js
CN=ca1
emailAddress=ry@tinyclouds.org`;

let infoAccessCheck = `OCSP - URI:http://ocsp.nodejs.org/
CA Issuers - URI:http://ca.nodejs.org/ca.cert\n`;

const legacyObjectCheck = {
  subject: {
    C: 'US',
    ST: 'CA',
    L: 'SF',
    O: 'Joyent',
    OU: 'Node.js',
    CN: 'agent1',
    emailAddress: 'ry@tinyclouds.org',
  },
  issuer: {
    C: 'US',
    ST: 'CA',
    L: 'SF',
    O: 'Joyent',
    OU: 'Node.js',
    CN: 'ca1',
    emailAddress: 'ry@tinyclouds.org',
  },
  infoAccess: {
    'OCSP - URI': ['http://ocsp.nodejs.org/'],
    'CA Issuers - URI': ['http://ca.nodejs.org/ca.cert']
  },
  modulus: 'D456320AFB20D3827093DC2C4284ED04DFBABD56E1DDAE529E28B790CD42' +
            '56DB273349F3735FFD337C7A6363ECCA5A27B7F73DC7089A96C6D886DB0C' +
            '62388F1CDD6A963AFCD599D5800E587A11F908960F84ED50BA25A28303EC' +
            'DA6E684FBE7BAEDC9CE8801327B1697AF25097CEE3F175E400984C0DB6A8' +
            'EB87BE03B4CF94774BA56FFFC8C63C68D6ADEB60ABBE69A7B14AB6A6B9E7' +
            'BAA89B5ADAB8EB07897C07F6D4FA3D660DFF574107D28E8F63467A788624' +
            'C574197693E959CEA1362FFAE1BBA10C8C0D88840ABFEF103631B2E8F5C3' +
            '9B5548A7EA57E8A39F89291813F45A76C448033A2B7ED8403F4BAA147CF3' +
            '5E2D2554AA65CE49695797095BF4DC6B',
  bits: 2048,
  exponent: '0x10001',
  valid_from: 'Sep  3 21:40:37 2022 GMT',
  valid_to: 'Jun 17 21:40:37 2296 GMT',
  fingerprint: '8B:89:16:C4:99:87:D2:13:1A:64:94:36:38:A5:32:01:F0:95:3B:53',
  fingerprint256:
    '2C:62:59:16:91:89:AB:90:6A:3E:98:88:A6:D3:C5:58:58:6C:AE:FF:9C:33:' +
    '22:7C:B6:77:D3:34:E7:53:4B:05',
  fingerprint512:
    '51:62:18:39:E2:E2:77:F5:86:11:E8:C0:CA:54:43:7C:76:83:19:05:D0:03:' +
    '24:21:B8:EB:14:61:FB:24:16:EB:BD:51:1A:17:91:04:30:03:EB:68:5F:DC:' +
    '86:E1:D1:7C:FB:AF:78:ED:63:5F:29:9C:32:AF:A1:8E:22:96:D1:02',
  serialNumber: '147D36C1C2F74206DE9FAB5F2226D78ADB00A426'
};

export const test_ok = {
  async test() {
    const x509 = new X509Certificate(cert);
    ok(!x509.ca);
    strictEqual(x509.subject, subjectCheck);
    strictEqual(x509.subjectAltName, undefined);
    strictEqual(x509.issuer, issuerCheck);
    strictEqual(x509.infoAccess, infoAccessCheck);
    strictEqual(x509.validFrom, 'Sep  3 21:40:37 2022 GMT');
    strictEqual(x509.validTo, 'Jun 17 21:40:37 2296 GMT');
    strictEqual(x509.fingerprint.length,
                '8B:89:16:C4:99:87:D2:13:1A:64:94:36:38:A5:32:01:F0:95:3B:53'.length);
    strictEqual(x509.fingerprint256,
                '2C:62:59:16:91:89:AB:90:6A:3E:98:88:A6:D3:C5:58:58:6C:AE:FF:9C:33:' +
                '22:7C:B6:77:D3:34:E7:53:4B:05');
    strictEqual(x509.fingerprint512,
                '0B:6F:D0:4D:6B:22:53:99:66:62:51:2D:2C:96:F2:58:3F:95:1C:CC:4C:44:' +
                '9D:B5:59:AA:AD:A8:F6:2A:24:8A:BB:06:A5:26:42:52:30:A3:37:61:30:A9:' +
                '5A:42:63:E0:21:2F:D6:70:63:07:96:6F:27:A7:78:12:08:02:7A:8B');
    strictEqual(x509.keyUsage, undefined);
    strictEqual(x509.serialNumber, '147D36C1C2F74206DE9FAB5F2226D78ADB00A426');

    deepStrictEqual(x509.raw, der);

    ok(x509.publicKey instanceof PublicKeyObject);
    ok(x509.publicKey.type === 'public');

    strictEqual(x509.toString().replaceAll('\r\n', '\n'),
                cert.toString().replaceAll('\r\n', '\n'));
    strictEqual(x509.toJSON(), x509.toString());

    // TODO(soon): Need to implement createPrivateKey this check
    // const privateKey = createPrivateKey(key);
    // ok(x509.checkPrivateKey(privateKey));
    throws(() => x509.checkPrivateKey(x509.publicKey), {
      code: 'ERR_INVALID_ARG_TYPE'
    });

    strictEqual(x509.checkIP('127.0.0.1'), undefined);
    strictEqual(x509.checkIP('::'), undefined);
    strictEqual(x509.checkHost('agent1'), 'agent1');
    strictEqual(x509.checkHost('agent2'), undefined);
    strictEqual(x509.checkEmail('ry@tinyclouds.org'), 'ry@tinyclouds.org');
    strictEqual(x509.checkEmail('sally@example.com'), undefined);
    ok(!x509.checkIssued(x509));

    throws(() => x509.checkHost('agent\x001'));
    throws(() => x509.checkIP('[::]'));
    throws(() => x509.checkEmail('not\x00hing'));

    [1, false].forEach((i) => {
      throws(() => x509.checkHost('agent1', i));
      throws(() => x509.checkHost('agent1', { subject: i }));
    });

    [
      'wildcards',
      'partialWildcards',
      'multiLabelWildcards',
      'singleLabelSubdomains',
    ].forEach((key) => {
      [1, '', {}].forEach((i) => {
        throws(() => x509.checkHost('agent1', { [key]: i }), {
          code: 'ERR_INVALID_ARG_TYPE'
        });
      });
    });

    const ca_cert = new X509Certificate(ca);
    ok(ca_cert.ca);
    ok(x509.checkIssued(ca_cert));

    ok(x509.verify(ca_cert.publicKey));

    throws(() => x509.checkIssued({}));
    throws(() => x509.checkIssued(''));
    throws(() => x509.verify({}));
    throws(() => x509.verify(''));
    throws(() => x509.verify(privateKey));

    const legacyObject = x509.toLegacyObject();
    deepStrictEqual(legacyObject.raw, x509.raw);

    deepStrictEqual(legacyObject.subject, legacyObjectCheck.subject);
    deepStrictEqual(legacyObject.issuer, legacyObjectCheck.issuer);
    deepStrictEqual(legacyObject.infoAccess, legacyObjectCheck.infoAccess);
    strictEqual(legacyObject.modulus, legacyObjectCheck.modulus);
    strictEqual(legacyObject.bits, legacyObjectCheck.bits);
    strictEqual(legacyObject.exponent, legacyObjectCheck.exponent);
    strictEqual(legacyObject.valid_from, legacyObjectCheck.valid_from);
    strictEqual(legacyObject.valid_to, legacyObjectCheck.valid_to);
    strictEqual(legacyObject.fingerprint, legacyObjectCheck.fingerprint);
    strictEqual(
      legacyObject.fingerprint256,
      legacyObjectCheck.fingerprint256);
    strictEqual(
      legacyObject.serialNumber,
      legacyObjectCheck.serialNumber);
  }
};

// This X.509 Certificate can be parsed by OpenSSL because it contains a
// structurally sound TBSCertificate structure. However, the SPKI field of the
// TBSCertificate contains the subjectPublicKey as a BIT STRING, and this bit
// sequence is not a valid public key. Ensure that X509Certificate.publicKey
// does not abort in this case.
const badCert = `-----BEGIN CERTIFICATE-----
MIIDpDCCAw0CFEc1OZ8g17q+PZnna3iQ/gfoZ7f3MA0GCSqGSIb3DQEBBQUAMIHX
MRMwEQYLKwYBBAGCNzwCAQMTAkdJMR0wGwYDVQQPExRQcml2YXRlIE9yZ2FuaXph
dGlvbjEOMAwGA1UEBRMFOTkxOTExCzAJBgNVBAYTAkdJMRIwEAYDVQQIFAlHaWJy
YWx0YXIxEjAQBgNVBAcUCUdpYnJhbHRhcjEgMB4GA1UEChQXV0hHIChJbnRlcm5h
dGlvbmFsKSBMdGQxHDAaBgNVBAsUE0ludGVyYWN0aXZlIEJldHRpbmcxHDAaBgNV
BAMUE3d3dy53aWxsaWFtaGlsbC5jb20wIhgPMjAxNDAyMDcwMDAwMDBaGA8yMDE1
MDIyMTIzNTk1OVowgbAxCzAJBgNVBAYTAklUMQ0wCwYDVQQIEwRSb21lMRAwDgYD
VQQHEwdQb21lemlhMRYwFAYDVQQKEw1UZWxlY29taXRhbGlhMRIwEAYDVQQrEwlB
RE0uQVAuUE0xHTAbBgNVBAMTFHd3dy50ZWxlY29taXRhbGlhLml0MTUwMwYJKoZI
hvcNAQkBFiZ2YXNlc2VyY2l6aW9wb3J0YWxpY29AdGVsZWNvbWl0YWxpYS5pdDCB
nzANBgkqhkiG9w0BAQEFAAOBjQA4gYkCgYEA5m/Vf7PevH+inMfUJOc8GeR7WVhM
CQwcMM5k46MSZo7kCk7VZuaq5G2JHGAGnLPaPUkeXlrf5qLpTxXXxHNtz+WrDlFt
boAdnTcqpX3+72uBGOaT6Wi/9YRKuCs5D5/cAxAc3XjHfpRXMoXObj9Vy7mLndfV
/wsnTfU9QVeBkgsCAwEAAaOBkjCBjzAdBgNVHQ4EFgQUfLjAjEiC83A+NupGrx5+
Qe6nhRMwbgYIKwYBBQUHAQwEYjBgoV6gXDBaMFgwVhYJaW1hZ2UvZ2lmMCEwHzAH
BgUrDgMCGgQUS2u5KJYGDLvQUjibKaxLB4shBRgwJhYkaHR0cDovL2xvZ28udmVy
aXNpZ24uY29tL3ZzbG9nbzEuZ2lmMA0GCSqGSIb3DQEBBQUAA4GBALLiAMX0cIMp
+V/JgMRhMEUKbrt5lYKfv9dil/f22ezZaFafb070jGMMPVy9O3/PavDOkHtTv3vd
tAt3hIKFD1bJt6c6WtMH2Su3syosWxmdmGk5ihslB00lvLpfj/wed8i3bkcB1doq
UcXd/5qu2GhokrKU2cPttU+XAN2Om6a0
-----END CERTIFICATE-----`;

export const test_error = {
  test() {
    throws(() => new X509Certificate(badCert));
  }
};
