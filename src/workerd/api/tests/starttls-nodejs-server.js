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
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQDIWfGy2tRsqANt
J1F/52bIDzMDxlmSkDpu3U3Ehq6TmH2hNBcLOWuWLvG8Np9artnzk8QnodfN8yEJ
0HRzZ6mRjVIUHJOb3+L1+0ePOM8dtWvG0AOd95K0T0imJRLPR18UjHl5OLE7mMS9
CGHa6mDTGKzTcdxtpkjiyoNgfdKKSKzLplga5if36leGJ2+mEhOAc/cV1kqOx+hP
VGAOz5p3OnkgolC8hZ3WTsAFEYMU0QoPNs7jVCVNGH9t3qPiWV2/7XaNQoMnMHgq
yyljcvDo0O0dw0HKtBVIMhz5xHHGZqJNM/R36MeHzO+cIYhJz+ncknu62+IlQOCY
eyHuxvpRAgMBAAECggEAB3SXYqsze+6doAZ2SS7se3XbVWDgbOyKjB0Wm4FShkIG
rMTCNcP3fbF2A+W5dNesWxzM0Be87thFCrcz2iaJoBW07/QnRwXkDXjCDzGTPX0G
i3GqrMptbmHD55DaHBYBEwPuMkVajQfwjEM/VvThUQGqTrz+MaNeM3hLPsA34Tbl
wigHK+4tyAFLkYkvsxXYHs1F23ey2ubFUyBI8gvfayOvr4MOVfEbQZsmz3IB5jjk
oK5EaMJuhty65pb9Pi6ncSbfVQ2aciNgHjZs/is/WQfQPB2jYBPpmaFQbZc2pseZ
8zTLvF+GKZng4hQE1F+F6DUf9sxb54Eu1XqzqGlrdQKBgQDkQc5fJj72rF9RkiBN
zeEK0Ngihm9Jzsb544i7DT/4jPWwa1+dt+kNVqguDbcxi4tCi+P8BwTnfX5M31d2
/Si9nDBpP+WLLRHjFq2AQf05JvFc1/7s4KvWBrfKbwiN+uxrstB2lDuFp386B20U
stsCBu/nawjt5lYY7S0zPokkJQKBgQDgs9rTJNnRuaaEpUxqRdR6zR3z0Qe1B1Ga
y6z8OqiX3N+wM9uAcrTzGOtPKvB4glcJZrrQp0NSxkJVsFBzPBCfUfjIV/IiOqS1
nE/rrEKEG7ZVkxDUsdeS8KiVKBJjLch/hrT0udgv4vndXqeaJuNzbD1yfiKbPnY5
yGC78uqvvQKBgGUGMx6twMRQekeSEzYcXvP4hxCQy4SxPiOvbv7K2HtbeApDG6ik
k0NSDVGExIXrKxGi9J7BRIxoYJQJbZ6+YV+6VzreCuxUYExP5y6TBk5bTAw5lRym
O6eYhZPVHMYqPqVUGSvCY629+nNmggLdPk1hYKDeIK+aeJTDtHOvw+b5AoGAZI74
wf8+34WWyMv026ZuhZpf6ipEqbYhxgWaX7KcmoHFNWSvudcbtaMUQ3Sy8ytZaiKo
PhJspZGGRDTIfBmIUtRrYrVA7iKSbZgLiCuqBNcmDTvoj1cbY24B8+Zf/DSUAsY1
G0RERIHuUiw3E1yN86yf/yoFsLYOUKOk7teyQX0CgYBFUzUeVszODI/1NeKuGdoR
1uJk2gt7hH4t7GdX4G78h3i6P5pa7qSyUXqBm53nXrYhEFRiVgDh5BoRnoKKWomj
IDidHZX3fMoQvdKAT8sUbT/Q3KKWPFqGv7frdpQe+JM0DwwjjPrMtUWhuve455XW
bCKgoxoaqEPUzY9CyI+RZg==
-----END PRIVATE KEY-----`;

  const cert = `-----BEGIN CERTIFICATE-----
MIIDmTCCAoGgAwIBAgIUFdaq1aN7zHoSsmLp6ItxnM1VOPEwDQYJKoZIhvcNAQEL
BQAwTjELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRlc3QxDTALBgNVBAcMBFRlc3Qx
DTALBgNVBAoMBFRlc3QxEjAQBgNVBAMMCWxvY2FsaG9zdDAeFw0yNTA3MjYwNjI3
MDZaFw0yNjA3MjYwNjI3MDZaME4xCzAJBgNVBAYTAlVTMQ0wCwYDVQQIDARUZXN0
MQ0wCwYDVQQHDARUZXN0MQ0wCwYDVQQKDARUZXN0MRIwEAYDVQQDDAlsb2NhbGhv
c3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDIWfGy2tRsqANtJ1F/
52bIDzMDxlmSkDpu3U3Ehq6TmH2hNBcLOWuWLvG8Np9artnzk8QnodfN8yEJ0HRz
Z6mRjVIUHJOb3+L1+0ePOM8dtWvG0AOd95K0T0imJRLPR18UjHl5OLE7mMS9CGHa
6mDTGKzTcdxtpkjiyoNgfdKKSKzLplga5if36leGJ2+mEhOAc/cV1kqOx+hPVGAO
z5p3OnkgolC8hZ3WTsAFEYMU0QoPNs7jVCVNGH9t3qPiWV2/7XaNQoMnMHgqyylj
cvDo0O0dw0HKtBVIMhz5xHHGZqJNM/R36MeHzO+cIYhJz+ncknu62+IlQOCYeyHu
xvpRAgMBAAGjbzBtMB0GA1UdDgQWBBQ6R0NLwjufqWjT75cFdzHW9bh2DDAfBgNV
HSMEGDAWgBQ6R0NLwjufqWjT75cFdzHW9bh2DDAPBgNVHRMBAf8EBTADAQH/MBoG
A1UdEQQTMBGCCWxvY2FsaG9zdIcEfwAAATANBgkqhkiG9w0BAQsFAAOCAQEAToR4
CaI9HAfSSXE+6fPthp+qrwPmfx3rW0RskpjKuqemZmIK7ydU9pcYuGtc6CPen614
RfFoaWtPltbNU0KV79P4zTRNYxqTKcEyhsjyAGbLA+bJtJE3hlDrfPGVyepZETXE
7Ig4XPyXi4M+WmvLboAF2dHC+H1XoWp3agIN45VRnr5uPVNX19dTbr0gc3WxLEUH
N839sKGVB9GVaQhF/4Z8ia0bluirf+6SNAaN/veJA40ixGEkHN3gqX4ZTZWl5rji
3cLdth83wmueKxiBp8ov78ubmdPiBsyIVrsb8jEsxRPAKX8gEx09S/yIIs1ZEGi9
wG73FlfFCc09zjmYww==
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
  .listen(process.env.STARTTLS_CA_PORT, () => {
    console.info(`ServerCA listening on port ${serverCA.address().port}`);
  });
