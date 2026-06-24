// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { connect } from 'cloudflare:sockets';
import { ok, strict as assert } from 'node:assert';
import { connect as tlsConnect, TLSSocket } from 'node:tls';
import { connect as netConnect } from 'node:net';
import unsafe from 'workerd:unsafe';

export const checkPortsSetCorrectly = {
  test(ctrl, env, ctx) {
    const keys = ['STARTTLS_CA_PORT'];
    for (const key of keys) {
      assert.strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
  },
};

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-35: tls.connect() must propagate
// options.servername to TLSSocket.servername and pass it as expectedServerHostname
// to the native Socket.startTls() call. Before the fix, servername was silently
// dropped, causing TLS certificate identity checks to use the transport host
// instead of the caller-specified server identity.
export const regressionServernamePassthrough = {
  async test(ctrl, env, ctx) {
    const opts = {
      servername: 'localhost',
      port: env.STARTTLS_CA_PORT,
      rejectUnauthorized: true,
    };

    const socket = netConnect(opts.port);

    await new Promise((resolve, reject) => {
      socket.once('data', (data) => {
        const greeting = data.toString().trim();
        if (greeting !== 'HELLO') {
          reject(new Error('Expected HELLO greeting'));
          return;
        }

        socket.write('HELLO_BACK\n');

        socket.once('data', (data) => {
          const signal = data.toString().trim();
          if (signal !== 'START_TLS') {
            reject(new Error('Expected START_TLS signal'));
            return;
          }

          // Upgrade to TLS with explicit servername
          const tlsSocket = tlsConnect(
            {
              ...opts,
              socket: socket,
            },
            function () {
              // After the fix, TLSSocket.servername must reflect the
              // caller-supplied value. Before the fix it was always null.
              assert.strictEqual(
                this.servername,
                'localhost',
                'TLSSocket.servername must be set to the caller-supplied servername'
              );

              this.write('ping\n');
              this.once('data', (data) => {
                assert.strictEqual(data.toString().trim(), 'pong');
                this.end();
                resolve();
              });
            }
          );

          // We use isTestAutogateEnabled() (which checks TEST_WORKERD) as a proxy
          // for whether STARTTLS_REJECT_EXPECTED_SERVER_HOSTNAME is enabled,
          // because the @all-autogates test variant enables every gate at once.
          if (unsafe.isTestAutogateEnabled()) {
            tlsSocket.on('error', (err) => {
              try {
                assert.match(err.message, /expectedServerHostname/);
                resolve();
              } catch (e) {
                reject(e);
              }
            });
          } else {
            tlsSocket.on('error', reject);
          }
        });
      });

      socket.on('error', reject);
    });
  },
};

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-35: setServername() must
// actually store the value so it is used during the TLS upgrade.  We construct
// a TLSSocket *without* a servername, call setServername('localhost') before
// the handshake, then trigger _start().  The server cert is issued for
// "localhost", so the handshake succeeds only if setServername() actually
// propagated the value to startTls({ expectedServerHostname }).
export const regressionSetServernameStoresValue = {
  async test(ctrl, env, ctx) {
    const socket = netConnect(env.STARTTLS_CA_PORT);

    await new Promise((resolve, reject) => {
      socket.once('data', (data) => {
        const greeting = data.toString().trim();
        if (greeting !== 'HELLO') {
          reject(new Error('Expected HELLO greeting'));
          return;
        }

        socket.write('HELLO_BACK\n');

        socket.once('data', (data) => {
          const signal = data.toString().trim();
          if (signal !== 'START_TLS') {
            reject(new Error('Expected START_TLS signal'));
            return;
          }

          // Create a TLSSocket with no servername — deliberately omitted so
          // that the only way the correct SNI reaches startTls() is through
          // our setServername() call below.
          const tlsSocket = new TLSSocket(socket, {
            rejectUnauthorized: true,
          });

          // This is the line under test: if setServername were a no-op the
          // handshake would either send no SNI or the wrong one, and the
          // server's certificate check for "localhost" would fail.
          tlsSocket.setServername('localhost');

          tlsSocket.on('secure', function () {
            try {
              assert.strictEqual(
                tlsSocket.servername,
                'localhost',
                'servername must reflect the value set via setServername()'
              );

              tlsSocket.write('ping\n');
              tlsSocket.once('data', (data) => {
                try {
                  assert.strictEqual(data.toString().trim(), 'pong');
                  tlsSocket.end();
                  resolve();
                } catch (e) {
                  reject(e);
                }
              });
            } catch (e) {
              reject(e);
            }
          });

          // We use isTestAutogateEnabled() (which checks TEST_WORKERD) as a proxy
          // for whether STARTTLS_REJECT_EXPECTED_SERVER_HOSTNAME is enabled,
          // because the @all-autogates test variant enables every gate at once.
          if (unsafe.isTestAutogateEnabled()) {
            tlsSocket.on('error', (err) => {
              try {
                assert.match(err.message, /expectedServerHostname/);
                resolve();
              } catch (e) {
                reject(e);
              }
            });
          } else {
            tlsSocket.on('error', reject);
          }

          tlsSocket._start();
        });
      });

      socket.on('error', reject);
    });
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
