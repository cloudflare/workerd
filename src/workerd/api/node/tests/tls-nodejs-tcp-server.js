// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This file is used as a sidecar for the tls-nodejs-test tests.
const tls = require('node:tls');

// Taken from https://github.com/nodejs/node/blob/304743655d5236c2edc39094336ee2667600b684/test/fixtures/keys/agent1-key.pem
const AGENT1_KEY_PEM = `-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCn5Ieb/G+/y5iD
MPWMJTjHhUDb4U5UiWyD69/jJ3W3tKPS62PaOHaOyuhd4aqNT0q3Eq3VhwlqkuoU
RUmE+8r7QkU9vnIcALf9/rvt8g9K1BAx+EUPjFwWOXgjCs/8Z5KPDrQ3cKxFTzkB
7TCcequLz/cKPet7lA16ZRe9d5QlhuUWqGlUuIAu78h0I7OZKrhE4bp5ZJcHGtO8
3JLp4CLxE28MZgzlxebvvLS+1fohoc1IFaxG0yxwYawPsjSFDaTLlM/IqL6imLUH
+KHrI9QIEWkrsNfqaTu5FlsKA5OrecFePw5Mgm65ljtRz+hISW2YFYT0GB07oaUl
c3HnNgLpAgMBAAECggEAGGIaR887s5EwDy2XG8l0G5YAu25XX/OtbONe2rCqagm0
GTfSgqjcnxRc9vWFPYycf0YZNP+toGrB0DvX15Zx/le7kqIMFIEON7c9N+uFyQpP
Z9J0xTNPVHL4Pa6eUjwAjwJFrh+RBWfiEaOPAcrXCzEi4bvobUQtSO9RqVSqkWBv
wEp6oI0gJvIpK7pRrUTCTR0qfFXTsaabvpWZvdmMtoDgiTRJhg2Dhvzy4Ox1y4/Z
fNKO/+KirKLS/FNje6b2LbzxFZ9YePDn0w3XD/iLaRduJ3UHNuMCl3tl3zVlgvKI
HhlCBnXcAbSWn3WqcQAUF57eRbwlt11za/gZi57DGQKBgQDRUtQthF9JleEoZ2YF
tQBanwz3AzEV2KHt3QrbjBxq3zhipx6K1iFr2SB6nDOigbJB/87gtoBdBbg9ujwj
2K3bO3FoQsCLTikXUJ5OD01vCY4RDAjddNVdnJEtbONEZOVSV1GGxtEzMsrJT1Gs
2LKmiSXEMRQhXJWgFaif8sFQ7wKBgQDNVKIS9Rh7ShSzXUhRwVmNvfHSiYEgeeGz
3mJpAUKMHp2WzWZrWfTOBfm4+2/dg7Q8dTE6okzS/I+6a3PPnB3lTSLYJmnI64DB
YccKKuxpk4zixxTWQk6YtJfBCyb16qWQoXAHBM7OVr3hd5egArLGNKxafERH8orX
kY2rtzs5pwKBgEZJX8Gg7zYQQ7iDb7h+3I2RVpMi2TqSsVzjmh+6Xlhsd8x4fUL1
P+es0sEY7iWlEywiL185KMUThJgFjugie85fmWb+8xRTvGx9v4pKjR+5v6BtwBRM
hNCYIA92vqFal74cX923qMteRMVwAubdJK/S4YGNUUsagYttel+q7cq1AoGAHvn4
pYGCWv83FkQpZ+QSfZa9R7Tk3SBmE3umPw8omfj4b0q3e9SLYRV3sheErddztnc7
oQvhKSdfC5GwXA7CV9iGPDO3W89jkkkM/RSyq87Nv1ynYReJwfHkvwPOseTfa21f
eD+ab3iYls4y+rnNfKdvpQsARhZqKdFUnSY8chsCgYBKx7zcgqSg+bdM9DB8Wzph
4ZQBLo3n2HZGpNTfT5UaKguYXqswQ5F/zPfxJBIwTAfL92bJnzRZpzVPcPg6QHhR
0DNeltg5Ij061Zyz2/RnDHS16uyQNENb4lcSlqdanlOsluVqE0GQC8y+Rvf+tA10
h0ttUVaTtmba2EGRzoaNWA==
-----END PRIVATE KEY-----`;

// Taken from https://github.com/nodejs/node/blob/304743655d5236c2edc39094336ee2667600b684/test/fixtures/keys/agent1-cert.pem
const AGENT1_CERT_PEM = `-----BEGIN CERTIFICATE-----
MIIDeDCCAmCgAwIBAgIUdzLeCZ2khgssLeLiVYURZo/Tx8EwDQYJKoZIhvcNAQEL
BQAwTTELMAkGA1UEBhMCVVMxEzARBgNVBAgMCk5ldyBKZXJzZXkxFDASBgNVBAcM
C0plcnNleSBDaXR5MRMwEQYDVQQKDApDbG91ZGZsYXJlMB4XDTI1MDMwNTE2MTUy
MVoXDTI3MDMwNTE2MTUyMVowRTELMAkGA1UEBhMCVVMxEzARBgNVBAgMClNvbWUt
U3RhdGUxITAfBgNVBAoMGEludGVybmV0IFdpZGdpdHMgUHR5IEx0ZDCCASIwDQYJ
KoZIhvcNAQEBBQADggEPADCCAQoCggEBAKfkh5v8b7/LmIMw9YwlOMeFQNvhTlSJ
bIPr3+Mndbe0o9LrY9o4do7K6F3hqo1PSrcSrdWHCWqS6hRFSYT7yvtCRT2+chwA
t/3+u+3yD0rUEDH4RQ+MXBY5eCMKz/xnko8OtDdwrEVPOQHtMJx6q4vP9wo963uU
DXplF713lCWG5RaoaVS4gC7vyHQjs5kquEThunlklwca07zckungIvETbwxmDOXF
5u+8tL7V+iGhzUgVrEbTLHBhrA+yNIUNpMuUz8iovqKYtQf4oesj1AgRaSuw1+pp
O7kWWwoDk6t5wV4/DkyCbrmWO1HP6EhJbZgVhPQYHTuhpSVzcec2AukCAwEAAaNY
MFYwFAYDVR0RBA0wC4IJbG9jYWxob3N0MB0GA1UdDgQWBBT1Uf2E3xOj0HPkG2e5
ZiCx0GSYKDAfBgNVHSMEGDAWgBRlkgGmcLtZzVTUu0MvdYI8I2gwCTANBgkqhkiG
9w0BAQsFAAOCAQEAuv0c58unZvUOK2nL007Vp/bjjfKf/rpsvlHKgZ2Oa4Xik/Io
O3Jh4/uTlTlq0GI5zSTQSdiI28yiKqGRmCtGkES9mjPzUkL+pTj/pFgyCjYfu/Z0
lIn1zwVEp1AXxhID3QvUDEeEM5+sE3i1KlGpF/0eYFJN6/ClS+VbJXmacdJATGnU
W3QKeel2BcFNDFreh8g+Ebb6K2P3k4+PHTWAuzcqu7eIEe4cKEjIDXI9vdckjq4Q
Nbc7EfWb2Gh/6S/Uz6y+AIN3l7ybJTqzWnOWZPCzoMpJuS+d5ir36DJ3BpXKCTHT
VRpM7av//cc39AeMvU6RTQSYVDy4NJx+mfRqLw==
-----END CERTIFICATE-----`;

const options = {
  key: AGENT1_KEY_PEM,
  cert: AGENT1_CERT_PEM,
  rejectUnauthorized: true,
};

const echoServer = tls.createServer(options, (s) => {
  s.setTimeout(100);
  s.on('error', () => {
    // Do nothing
  });
  s.pipe(s);
});
echoServer.listen(8888, () => console.info('Listening on port 8888'));

// Taken from test-tls-connect-given-socket.js
const helloServer = tls.createServer(options, (socket) => {
  socket.end('Hello');
});
helloServer.listen(8887, () => console.info('Listening on port 8887'));
