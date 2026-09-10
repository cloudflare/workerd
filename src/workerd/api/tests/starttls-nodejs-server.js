// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/*
 * This file serves a server that tries to upgrade tls connections and send messages.
 * This file is designed to run as a sidecar
 */

const net = require('node:net');
const tls = require('node:tls');
const assert = require('node:assert');

// Create a self-signed certificate for TLS with proper SAN extension
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

const serverCA = net
  .createServer((s) => {
    console.log('ServerCA: New connection received');

    // Send initial greeting
    s.write('HELLO\n');

    // Wait for one response then upgrade to TLS
    s.once('data', (data) => {
      const response = data.toString().trim();
      console.log('ServerCA: Received response:', response);

      if (response === 'HELLO_BACK') {
        s.write('START_TLS\n', () => {
          console.log('ServerCA: Sent START_TLS, upgrading to TLS');

          // Small delay to ensure START_TLS is sent
          console.log('serverCA: Creating TLS socket');
          const tlsSocket = new tls.TLSSocket(s, {
            isServer: true,
            server: serverCA,
            secureContext: tls.createSecureContext(createSelfSignedCert()),
            requestCert: false,
            SNICallback: (hostname, callback) => {
              console.log('serverCA: SNI callback for hostname:', hostname);
              assert.strictEqual(hostname, 'localhost');
              callback(null, null);
            },
          });

          console.log('serverCA: Setting up TLS event handlers');

          tlsSocket.on('secure', () => {
            console.log('serverCA: TLS handshake complete');

            // Handle TLS data
            tlsSocket.on('data', (data) => {
              const message = data.toString().trim();
              console.log('serverCA: Received TLS message:', message);

              if (message === 'ping') {
                console.log('serverCA: Sending pong response');
                tlsSocket.write('pong\n', (err) => {
                  if (err) {
                    console.log('serverCA: Error writing pong:', err);
                  } else {
                    console.log('serverCA: Pong sent successfully');
                  }
                });
              }
            });
          });

          tlsSocket.on('error', (err) => {
            console.log('ServerCA TLS error:', err.message);
          });

          tlsSocket.on('close', () => {
            console.log('serverCA: TLS socket closed');
          });

          console.log('serverCA: TLS socket created, waiting for handshake');

          // The TLS handshake should start when the client initiates it
        });
      }
    });

    s.on('error', (err) => {
      console.log('ServerCA socket error:', err.message);
    });
  })
  .listen(0, () => {
    console.log(`STARTTLS_CA_PORT=${serverCA.address().port}`);
  });
