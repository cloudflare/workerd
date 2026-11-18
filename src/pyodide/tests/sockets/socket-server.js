// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// copied from: src/workerd/api/tests/http-socket-server.js

import net from 'node:net';
import tls from 'node:tls';

// Servers that need a hostname bind here: `localhost` resolves to this address and the
// certificate below covers it. SIDECAR_HOSTNAME is a randomized 127.x.x.x address with no
// hostname, so it can only be reached by IP.
const LOOPBACK_ADDRESS = '127.0.0.1';

const tlsOptions = {
  key: `-----BEGIN PRIVATE KEY-----
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
-----END PRIVATE KEY-----`,
  cert: `-----BEGIN CERTIFICATE-----
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
-----END CERTIFICATE-----`,
};

function logSocketErrors(socket, label) {
  // A client may close abruptly, but it's not an error we need to handle or a test failure
  // just log it.
  socket.on('error', (err) => {
    console.log(`${label} socket error: ${err.code ?? err.message}`);
  });
}

function handlePlainConnection(socket) {
  logSocketErrors(socket, 'plain');

  let buffer = Buffer.alloc(0);
  let pendingCount = null;

  socket.on('data', (data) => {
    buffer = Buffer.concat([buffer, data]);

    while (true) {
      if (pendingCount !== null) {
        if (buffer.length < pendingCount) {
          return;
        }

        buffer = buffer.subarray(pendingCount);
        socket.write(`COUNTED ${pendingCount}\n`);
        socket.end();
        return;
      }

      const newline = buffer.indexOf(0x0a);
      if (newline === -1) {
        return;
      }

      const line = buffer.subarray(0, newline).toString('utf8');
      buffer = buffer.subarray(newline + 1);

      if (line.startsWith('BASIC ')) {
        socket.write(`echo: ${line.slice('BASIC '.length)}\n`);
        socket.end();
        return;
      }

      if (line.startsWith('ECHO ')) {
        socket.write(`${line.slice('ECHO '.length)}\n`);
        continue;
      }

      if (line === 'BYE') {
        socket.end();
        return;
      }

      if (line === 'FINAL') {
        socket.write('final message\n');
        socket.end();
        return;
      }

      if (line === 'LINES') {
        socket.write('line1\nline2\nline3\n');
        socket.end();
        return;
      }

      if (line.startsWith('COUNT ')) {
        pendingCount = Number(line.slice('COUNT '.length));
        continue;
      }

      if (line.startsWith('SEND ')) {
        const size = Number(line.slice('SEND '.length));
        socket.write(Buffer.alloc(size, 'y'));
        socket.end();
        return;
      }

      socket.destroy(new Error(`unknown command: ${line}`));
      return;
    }
  });
}

const server = net.createServer(handlePlainConnection);

server.listen(0, process.env.SIDECAR_HOSTNAME, () => {
  console.log(`PYTHON_SOCKET_SERVER_PORT=${server.address().port}`);
});

const hostnameServer = net.createServer(handlePlainConnection);

hostnameServer.listen(0, LOOPBACK_ADDRESS, () => {
  console.log(
    `PYTHON_HOSTNAME_SOCKET_SERVER_PORT=${hostnameServer.address().port}`
  );
});

const tlsServer = tls.createServer(tlsOptions, (socket) => {
  logSocketErrors(socket, 'tls');

  let buffer = Buffer.alloc(0);

  socket.on('data', (data) => {
    buffer = Buffer.concat([buffer, data]);

    const newline = buffer.indexOf(0x0a);
    if (newline === -1) {
      return;
    }

    const line = buffer.subarray(0, newline).toString('utf8');
    if (line.startsWith('BASIC ')) {
      socket.write(`tls echo: ${line.slice('BASIC '.length)}\n`);
      socket.end();
      return;
    }

    socket.destroy(new Error(`unknown TLS command: ${line}`));
  });
});

tlsServer.listen(0, LOOPBACK_ADDRESS, () => {
  console.log(`PYTHON_TLS_SOCKET_SERVER_PORT=${tlsServer.address().port}`);
});

const startTlsServer = net.createServer((socket) => {
  logSocketErrors(socket, 'starttls');

  socket.write('HELLO\n');

  socket.once('data', (data) => {
    if (data.toString('utf8').trim() !== 'HELLO_BACK') {
      socket.destroy(new Error(`unexpected STARTTLS response: ${data}`));
      return;
    }

    socket.write('START_TLS\n', () => {
      const tlsSocket = new tls.TLSSocket(socket, {
        isServer: true,
        secureContext: tls.createSecureContext(tlsOptions),
        requestCert: false,
      });
      logSocketErrors(tlsSocket, 'starttls-tls');

      let buffer = Buffer.alloc(0);
      tlsSocket.on('data', (tlsData) => {
        buffer = Buffer.concat([buffer, tlsData]);

        const newline = buffer.indexOf(0x0a);
        if (newline === -1) {
          return;
        }

        const line = buffer.subarray(0, newline).toString('utf8');
        if (line === 'ping') {
          tlsSocket.write('pong\n');
          tlsSocket.end();
          return;
        }

        tlsSocket.destroy(new Error(`unknown STARTTLS command: ${line}`));
      });
    });
  });
});

startTlsServer.listen(0, LOOPBACK_ADDRESS, () => {
  console.log(
    `PYTHON_STARTTLS_SOCKET_SERVER_PORT=${startTlsServer.address().port}`
  );
});
