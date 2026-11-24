// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Answers every request with a response whose header value contains a NUL byte, so that the
// response head doesn't parse. Also serves TLS with a self-signed certificate that the test's
// `internet` service doesn't trust.

const net = require('net');
const tls = require('tls');

const server = net.createServer((socket) => {
  let request = '';
  socket.on('data', (chunk) => {
    request += chunk.toString('latin1');
    if (request.includes('\r\n\r\n')) {
      socket.end('HTTP/1.1 200 OK\r\nX-Bad: x\0x\r\nContent-Length: 0\r\n\r\n');
    }
  });
  socket.on('error', () => {});
});

server.listen({ port: 0, host: process.env.SIDECAR_HOSTNAME }, () => {
  console.log(`INVALID_RESPONSE_SERVER_PORT=${server.address().port}`);
});

// The self-signed certificate of http-socket-server.js.
function createSelfSignedCert() {
  const key = `-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCmi4tGNwMie1Ha
IYThyrfGHUYFTrgWXTSmqNHVaJYNc0DccJBb5+COO3XvN21hkLyaq7/BLGxZuDRf
ZUDbrD7Ljlp37UlT9elnjBIUTkciVCSuO8WWZxXCGHJBOXz0Lu+Yn8JSaf0haBmW
k7NBiM1q7y7iyArXb2vxWhgwesJoxqqQAKhOaK+w46qK1LRzAps5nKQSPQ4s6TTV
nEKH8gPSbb8CO5mZi31yX2gTmdnNkcaCkGZpMzfTvM1WhcFEWiIxgpuzzbJ6bM8g
aD4PHZtH5py5/tq1qJn0SVWV+5APRGHL2wuRmOincJJ/Z7gaLEwzJYfAON1RY4nW
yqJ45PNxAgMBAAECggEAClZ3GrCnfShyeDvEZ8+RgLaodgPg4WJ/tiicfca0KbmR
uR0zmMDX63w9Yc/q9jdpvaATkwYS467PcWxzji8u5qu7ad5Mwu1sw4SqSqBhQfw/
GqGTLjbP3vBthybuXqnbLXodMchqcfxoOjSI1/SJ661VF7abFaxQ4vZYsgmsr5wr
r9DL4ZXl0v5+vQciThfwnfPhLOVj8yQQsYP9YFR147aMPVDbw6V34+xTWOlr8g1s
0pThbq+4J2vMorZL/T5NaiAAxtQNaXfOth4Gmo88yrVUKjCNG8tOQ23iFOe8JEna
r3IrkeS5ppncGf219D3NnLZvVhRKRv99gHPc020/EQKBgQDPQ1V/bMuUdLYB4kzK
/GcCePrtIqgTLtxTNTypFLUijacwU+U4AiLBrkejpuK4DrvNc8lwhYP4lLgeC8ll
kRHOf57jymNhZs1Z4JXtxNCaX8EGNDH7xZiszlzBzuOVNN9ghN3OWuzU2Mt6GZJf
hiAv0g6rsEBmG1F+WRSZ7Lv6/wKBgQDNtRowCAPYeRMefmAum/kQ9LTmdlfCUmA2
GusfWHCYQjsbZseWa8irEJo0XM79yFChtdAFhfjSjpCjTZJnDoNQZUb3Ks9+SmvY
De83VUK5S7HeCqntAyq7IqR9c2h28zB1vf/ReRueS8fqFy612Ab1tSk7bqQd5Sgq
NF/dS9dBjwKBgHkfNWi5EKOaLP8e25fINv6X6rQIC8biHLm3o9J/mnct5uV0McEw
ZlVfXthBX78GRTQElVTfgccUSiCs7K4hQBG4PQeLr9Ys+Jasi5Ge8fU1Ph09BXTH
/bgHBOfx2sfIVT4Xh3PfaQXeB9M7/HE8dbTcgdxNrOS+1DoNHt/xG83LAoGADLfp
ypZ0RmoV+IivwbH7EEVQ+f7PJkCZmj7H0sRREdjmdqdAJ9i6K3l3T019rss30QfA
uNazr7EI2E/vgVewXsQkQxvugExxpoYWCEHJQlOfx665GuJbPf7CVM9R4ijfEiiR
LI5kWsEstxh/1tZod8CfsAEDPKXyecmLM8+Am5kCgYEAlc5G3XbbUHWKiKUZtGFc
WBtRdLX3UwF7Q2e3HYBS9uf1LtZuBPP4Xx/kwxS54R4xzXuBMnKLL3/S4+5YWZ8Q
HP4QxJfOyECHCRXLYQ+uViLWcjAs8u1IOtVEadzeITTm7WtKhfulJaEYuHhOdfs7
khOrIfcJKK8Dqqza/3yLPbc=
-----END PRIVATE KEY-----`;

  const cert = `-----BEGIN CERTIFICATE-----
MIIDmzCCAoOgAwIBAgIUcHX4QyPPWolmDciVOKeleSmshL8wDQYJKoZIhvcNAQEL
BQAwTjELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRlc3QxDTALBgNVBAcMBFRlc3Qx
DTALBgNVBAoMBFRlc3QxEjAQBgNVBAMMCWxvY2FsaG9zdDAgFw0yNjA3MjcwNDE1
NTNaGA8yMTI2MDcwMzA0MTU1M1owTjELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRl
c3QxDTALBgNVBAcMBFRlc3QxDTALBgNVBAoMBFRlc3QxEjAQBgNVBAMMCWxvY2Fs
aG9zdDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAKaLi0Y3AyJ7Udoh
hOHKt8YdRgVOuBZdNKao0dVolg1zQNxwkFvn4I47de83bWGQvJqrv8EsbFm4NF9l
QNusPsuOWnftSVP16WeMEhRORyJUJK47xZZnFcIYckE5fPQu75ifwlJp/SFoGZaT
s0GIzWrvLuLICtdva/FaGDB6wmjGqpAAqE5or7DjqorUtHMCmzmcpBI9DizpNNWc
QofyA9JtvwI7mZmLfXJfaBOZ2c2RxoKQZmkzN9O8zVaFwURaIjGCm7PNsnpszyBo
Pg8dm0fmnLn+2rWomfRJVZX7kA9EYcvbC5GY6Kdwkn9nuBosTDMlh8A43VFjidbK
onjk83ECAwEAAaNvMG0wHQYDVR0OBBYEFJ61qsbjnOYlrUGSvrnP1wMNANJOMB8G
A1UdIwQYMBaAFJ61qsbjnOYlrUGSvrnP1wMNANJOMBoGA1UdEQQTMBGCCWxvY2Fs
aG9zdIcEfwAAATAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQBx
v55hVThgDaxEHvgwHm1rfc0JdNQBxICOuEpIVj/epe++kaSQos+pEKsl/hBJG/pC
13hAyF+Ya9s5tyPinKbuFLZRyhdMS1O1zQrWvzZq3pjRRPt5ogvbwmKYGonMQnnp
8MpJa09bfdk5ZaIVO2THhtHjhzpYpKcGW4khXzCPmNTtX7Kiur0fhfkI0Ru4HVI+
BIemgjJPd01kGo1v0IK8FncbRuNDU6bDVAHinsOvSU+z2lA+cy2mN0/n44INwkHc
Zq+ggAjGTjKLo/tR37gabSUA20BKrpf5mqkMkoMRuBQ+aFdJmSNMZBR3z842nyE3
01KcXSMf3w+PAyt8iNZm
-----END CERTIFICATE-----`;

  return { key, cert };
}

const untrustedTlsServer = tls.createServer(createSelfSignedCert(), (socket) =>
  socket.end()
);
untrustedTlsServer.on('tlsClientError', () => {});
untrustedTlsServer.listen(
  { port: 0, host: process.env.SIDECAR_HOSTNAME },
  () => {
    console.log(
      `UNTRUSTED_TLS_SERVER_PORT=${untrustedTlsServer.address().port}`
    );
  }
);
