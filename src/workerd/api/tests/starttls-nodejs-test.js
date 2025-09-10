// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { connect } from 'cloudflare:sockets';
import { ok, strict as assert } from 'node:assert';
import { connect as tlsConnect } from 'node:tls';
import { connect as netConnect } from 'node:net';

export const checkPortsSetCorrectly = {
  test(ctrl, env, ctx) {
    const keys = ['STARTTLS_CA_PORT'];
    for (const key of keys) {
      assert.strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
  },
};

export const startTlsCATest = {
  async test(ctrl, env, ctx) {
    const opts = {
      servername: 'localhost',
      port: env.STARTTLS_CA_PORT,
      rejectUnauthorized: true,
    };

    const socket = netConnect(opts.port);

    // Wait for server's greeting then respond
    socket.once('data', (data) => {
      const greeting = data.toString().trim();
      console.log('startTlsCATest: Received greeting:', greeting);

      if (greeting === 'HELLO') {
        console.log('startTlsCATest: Sending HELLO_BACK');
        socket.write('HELLO_BACK\n');

        // Wait for START_TLS signal
        socket.once('data', (data) => {
          const signal = data.toString().trim();
          console.log('startTlsCATest: Received signal:', signal);

          if (signal === 'START_TLS') {
            console.log('startTlsCATest: Received START_TLS, upgrading to TLS');

            // Upgrade to TLS - pass socket in the options
            const tlsSocket = tlsConnect(
              {
                ...opts,
                socket: socket,
              },
              function () {
                console.log(
                  'startTlsCATest: TLS connection established successfully'
                );

                // Send ping message
                console.log('startTlsCATest: Sending ping message');
                this.write('ping\n', (err) => {
                  if (err) {
                    console.log('startTlsCATest: Error writing ping:', err);
                  } else {
                    console.log('startTlsCATest: Ping sent successfully');
                  }
                });

                // Wait for pong response
                this.once('data', (data) => {
                  const response = data.toString().trim();
                  console.log('startTlsCATest: Received response:', response);

                  // Assert response is 'pong'
                  assert.strictEqual(
                    response,
                    'pong',
                    'Expected pong response'
                  );

                  this.end();
                });
              }
            );

            tlsSocket.on('error', (err) => {
              console.log('startTlsCATest: TLS connection error:', err.message);
              throw err;
            });
          }
        });
      }
    });

    socket.on('error', (err) => {
      console.log('startTlsCATest: Socket connection error:', err.message);
      throw err;
    });
  },
};

export const startTlsCloudflareTest = {
  async test(ctrl, env, ctx) {
    // Create a Cloudflare socket connection with STARTTLS
    const socket = connect(`localhost:${env.STARTTLS_CA_PORT}`, {
      secureTransport: 'starttls',
    });

    const writer = socket.writable.getWriter();
    const reader = socket.readable.getReader();

    try {
      // Read HELLO greeting
      const { value: greeting } = await reader.read();
      const greetingText = new TextDecoder().decode(greeting).trim();
      console.log('startTlsCfTest: Received greeting:', greetingText);

      if (greetingText === 'HELLO') {
        console.log('startTlsCfTest: Sending HELLO_BACK');
        await writer.write(new TextEncoder().encode('HELLO_BACK\n'));

        // Read START_TLS signal
        const { value: signal } = await reader.read();
        const signalText = new TextDecoder().decode(signal).trim();
        console.log('startTlsCfTest: Received signal:', signalText);

        if (signalText === 'START_TLS') {
          console.log('startTlsCfTest: Received START_TLS, upgrading to TLS');

          // Release the reader and writer before upgrading
          reader.releaseLock();
          writer.releaseLock();

          // Upgrade to TLS using Cloudflare socket's startTls
          console.log('startTlsCfTest: About to start TLS');
          const tlsSocket = socket.startTls();
          console.log('startTlsCfTest: Started TLS');

          await tlsSocket.opened;
          console.log(
            'startTlsCfTest: TLS connection established successfully'
          );

          // Get new writer and reader for TLS socket
          const tlsWriter = tlsSocket.writable.getWriter();
          const tlsReader = tlsSocket.readable.getReader();

          // Send ping message
          console.log('startTlsCfTest: Sending ping message');
          await tlsWriter.write(new TextEncoder().encode('ping\n'));

          // Read pong response
          const { value: response } = await tlsReader.read();
          const responseText = new TextDecoder().decode(response).trim();
          console.log('startTlsCfTest: Received response:', responseText);

          // Assert response is 'pong'
          assert.strictEqual(responseText, 'pong', 'Expected pong response');

          // Close the socket
          tlsReader.releaseLock();
          tlsWriter.releaseLock();
          await tlsSocket.close();
        }
      }
    } catch (err) {
      console.log('startTlsCfTest: Error:', err.message);
      throw err;
    }
  },
};
