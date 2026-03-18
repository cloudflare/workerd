import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';
import assert from 'node:assert';
import { scheduler } from 'node:timers/promises';

// 5s timeout for some of the requests going to the container.
// We can get to have a stack trace with an
// abort signal.
const DEFAULT_TIMEOUT_DURATION = 10_000;

// Use a unique DO name per test invocation because different test flavors may
// run concurrently, and this avoids them accidentally sharing the same object.
function getRandomDurableObjectName(name) {
  return `${name}-${crypto.randomUUID()}`;
}

// **IMPORTANT NOTE**
//
// When writing a test, don't forget to call waitUntilContainerIsHealthy
// before testing the behaviour with your container.
//
// Don't forget to call monitor() after calling start(), as there
// is an issue with not calling monitor() in Durable Objects where
// we might lose track of the container lifetime.
//

export class DurableObjectExample extends DurableObject {
  async testExitCode() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }
    assert.strictEqual(container.running, false);

    // Start container with invalid entrypoint
    {
      container.start({
        entrypoint: ['node', 'nonexistant.js'],
      });

      let exitCode = undefined;
      await container.monitor().catch((err) => {
        exitCode = err.exitCode;
      });

      assert.strictEqual(typeof exitCode, 'number');
      assert.notEqual(0, exitCode);
    }

    // Start container with valid entrypoint and stop it
    {
      container.start();

      await scheduler.wait(500);

      let exitCode = undefined;
      const monitor = container.monitor().catch((err) => {
        exitCode = err.exitCode;
      });
      await container.destroy();
      await monitor;

      assert.strictEqual(typeof exitCode, 'number');
      assert.equal(137, exitCode);
    }
  }

  async testBasics() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});

      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    // Start container with valid configuration
    container.start({
      env: { A: 'B', C: 'D', L: 'F' },
      enableInternet: true,
    });

    const monitor = container.monitor().catch((_err) => {});

    await this.waitUntilContainerIsHealthy();

    await container.destroy();

    await monitor;
    assert.strictEqual(container.running, false);
  }

  async testSetInactivityTimeout(timeout) {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }
    assert.strictEqual(container.running, false);

    container.start();

    assert.strictEqual(container.running, true);

    // Wait for container to be running
    await scheduler.wait(500);

    try {
      await container.setInactivityTimeout(0);
    } catch (err) {
      assert.strictEqual(err.name, 'TypeError');
      assert.match(
        err.message,
        /setInactivityTimeout\(\) cannot be called with a durationMs <= 0/
      );
    }

    if (timeout > 0) {
      await container.setInactivityTimeout(timeout);
    }
  }

  async start() {
    assert.strictEqual(this.ctx.container.running, false);
    this.ctx.container.start();
    assert.strictEqual(this.ctx.container.running, true);

    // Wait for container to be running
    await scheduler.wait(500);
  }

  // Assert that the container is running
  async expectRunning(running) {
    assert.strictEqual(this.ctx.container.running, running);
    await this.ctx.container.destroy();
  }

  async abort() {
    await this.ctx.storage.put('aborted', true);
    await this.ctx.storage.sync();
    this.ctx.abort();
  }

  async alarm() {
    const alarmValue = (await this.ctx.storage.get('alarm')) ?? 0;

    const aborted = await this.ctx.storage.get('aborted');
    assert.strictEqual(!!this.ctx.container, true);
    if (aborted) {
      await this.ctx.storage.put('aborted-confirmed', true);
    }

    await this.ctx.storage.put('alarm', alarmValue + 1);
  }

  async getAlarmIndex() {
    return (await this.ctx.storage.get('alarm')) ?? 0;
  }

  async startAlarm(start, ms) {
    if (start && !this.ctx.container.running) {
      this.ctx.container.start();
    }
    await this.ctx.storage.setAlarm(Date.now() + ms);
  }

  async checkAlarmAbortConfirmation() {
    const abortConfirmation = await this.ctx.storage.get('aborted-confirmed');
    if (!abortConfirmation) {
      throw new Error(
        `Abort confirmation did not get inserted: ${abortConfirmation}`
      );
    }
  }

  async testWs() {
    const { container } = this.ctx;

    if (!container.running) {
      container.start({
        env: { WS_ENABLED: 'true' },
        enableInternet: true,
      });
    }

    await this.waitUntilContainerIsHealthy();

    const res = await container.getTcpPort(8080).fetch('http://foo/ws', {
      headers: {
        Upgrade: 'websocket',
        Connection: 'Upgrade',
        'Sec-WebSocket-Key': 'x3JJHMbDL1EzLkh9GBhXDw==',
        'Sec-WebSocket-Version': '13',
      },
      signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
    });

    // Should get WebSocket upgrade response
    assert.strictEqual(res.status, 101);
    assert.strictEqual(res.headers.get('upgrade'), 'websocket');
    assert.strictEqual(!!res.webSocket, true);

    // Test basic WebSocket communication
    const ws = res.webSocket;
    ws.accept();

    // Listen for response
    const messagePromise = new Promise((resolve) => {
      ws.addEventListener(
        'message',
        (event) => {
          resolve(event.data);
        },
        { once: true }
      );
    });

    // Send a test message
    ws.send('Hello WebSocket!');

    assert.strictEqual(await messagePromise, 'Echo: Hello WebSocket!');

    ws.close();
    await container.destroy();
  }

  getStatus() {
    return this.ctx.container.running;
  }

  async waitUntilContainerIsHealthy() {
    const container = this.ctx.container;
    {
      let resp;
      // The retry count here is arbitrary. Can increase it if necessary.
      const maxRetries = 15;
      for (let i = 1; i <= maxRetries; i++) {
        try {
          resp = await container.getTcpPort(8080).fetch('http://foo/bar/baz', {
            method: 'POST',
            body: 'hello',
            signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
          });
          break;
        } catch (e) {
          if (!e.message.includes('Container is not listening to port 8080')) {
            console.error(
              'Error querying getTcpPort().fetch() that is not related to the container not listening yet',
              e.message
            );

            throw e;
          }

          if (i === maxRetries) {
            console.error(
              `Failed to connect to container ${container.id}. Retried ${i} times`
            );
            throw e;
          }

          await scheduler.wait(500);
        }
      }

      assert.equal(resp.status, 200);
      assert.equal(resp.statusText, 'OK');
      assert.strictEqual(await resp.text(), 'Hello World!');
    }
  }

  async fetchIntercept(host) {
    return await this.ctx.container
      .getTcpPort(8080)
      .fetch('http://foo/intercept', {
        headers: { 'x-host': host },
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
  }

  async expectIntercept(host, expectedStatus, expectedBody) {
    const response = await this.fetchIntercept(host);
    assert.equal(response.status, expectedStatus);
    assert.equal(await response.text(), expectedBody);
  }

  async testPortNotListening() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start();
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await assert.rejects(
      container.getTcpPort(8081).fetch('http://foo/bar', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      }),
      /Container is not listening to port 8081/
    );

    await container.destroy();
    await monitor;
  }

  async testPidNamespace() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    container.start({
      enableInternet: true,
    });

    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const resp = await container
      .getTcpPort(8080)
      .fetch('http://foo/pid-namespace', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    assert.equal(resp.status, 200);
    const data = await resp.json();

    await container.destroy();
    await monitor;
    assert.strictEqual(container.running, false);

    return data;
  }

  async testSetEgressHttpWithInternet() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });

    await this.waitUntilContainerIsHealthy();

    await container.interceptOutboundHttp(
      'googlefakedomain.com',
      this.ctx.exports.TestService({ props: { id: 2 } })
    );

    await this.expectIntercept(
      'googlefakedomain.com',
      200,
      'hello binding: 2 http://googlefakedomain.com/'
    );

    await this.expectIntercept(
      'googlefakedomainother.com',
      500,
      'googlefakedomainother.com fetch failed'
    );

    await container.interceptAllOutboundHttp(
      this.ctx.exports.TestService({ props: { id: 5 } })
    );

    await this.expectIntercept(
      'google.com',
      200,
      'hello binding: 5 http://google.com/'
    );
  }

  async testSetEgressHttpNoInternet() {
    const container = this.ctx.container;

    if (!container.running) container.start();

    // wait for container to be available
    await this.waitUntilContainerIsHealthy();

    await container.interceptOutboundHttp(
      'google.com',
      this.ctx.exports.TestService({ props: { id: 2 } })
    );

    await this.expectIntercept(
      'google.com',
      200,
      'hello binding: 2 http://google.com/'
    );

    // This should fail as there is no hostname that matches it.
    await this.expectIntercept('google2.com', 500, 'google2.com fetch failed');

    await container.interceptOutboundHttp(
      'google2.com',
      this.ctx.exports.TestService({ props: { id: 4 } })
    );

    await this.expectIntercept(
      'google.com',
      200,
      'hello binding: 2 http://google.com/'
    );
    await this.expectIntercept(
      'google2.com',
      200,
      'hello binding: 4 http://google2.com/'
    );

    // From now on, all hostnames resolve to Workerd.
    await container.interceptAllOutboundHttp(
      this.ctx.exports.TestService({ props: { id: 6 } })
    );

    await this.expectIntercept(
      'google.com',
      200,
      'hello binding: 2 http://google.com/'
    );

    await this.expectIntercept(
      'google2.com',
      200,
      'hello binding: 4 http://google2.com/'
    );

    await this.expectIntercept(
      'google3.com',
      200,
      'hello binding: 6 http://google3.com/'
    );

    await this.expectIntercept(
      '1.1.1.1',
      200,
      'hello binding: 6 http://1.1.1.1/'
    );

    await this.expectIntercept('1.1.1.1:90', 500, '1.1.1.1:90 fetch failed');
    await this.expectIntercept(
      'google.com:9000',
      500,
      'google.com:9000 fetch failed'
    );
  }

  async createSnapshotForTransfer() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const writeResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/cross-do.txt', {
        method: 'POST',
        body: 'cross-do-snapshot',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(writeResp.status, 200);

    const snapshot = await container.snapshotDirectory({
      dir: '/app/data',
      name: 'cross-do-snapshot',
    });

    await container.destroy();
    await monitor;

    return snapshot;
  }

  async restoreTransferredSnapshot(snapshot) {
    assert.ok(snapshot.id, 'snapshot must have a non-empty id');
    assert.ok(snapshot.size > 0, 'snapshot must have a positive size');
    assert.strictEqual(snapshot.dir, '/app/data');

    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({
      enableInternet: true,
      snapshots: [{ snapshot }],
    });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const readResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/cross-do.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(readResp.status, 200);
    assert.strictEqual(await readResp.text(), 'cross-do-snapshot');

    await container.destroy();
    await monitor;
  }

  async testSetEgressHttp() {
    const container = this.ctx.container;

    // Set up egress TCP mapping to route requests to the binding
    // We can configure this even before the container starts.
    await container.interceptOutboundHttp(
      '1.2.3.4',
      this.ctx.exports.TestService({ props: { id: 1234 } })
    );

    if (!container.running) container.start();

    // Keep container alive after abort();
    container.monitor().catch((err) => {
      console.error('Container exited with an error:', err.message);
    });

    // wait for container to be available
    await this.waitUntilContainerIsHealthy();

    // Set up egress TCP mapping to route requests to the binding
    // This registers the binding's channel token with the container runtime
    await container.interceptOutboundHttp(
      '11.0.0.1:9999',
      this.ctx.exports.TestService({ props: { id: 1 } })
    );

    await container.interceptOutboundHttp(
      '11.0.0.2:9999',
      this.ctx.exports.TestService({ props: { id: 2 } })
    );

    // we catch all http requests to port 80
    await container.interceptAllOutboundHttp(
      this.ctx.exports.TestService({ props: { id: 3 } })
    );

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '1.2.3.4:80' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 1234 http://1.2.3.4/'
      );
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '11.0.0.1:9999' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 1 http://11.0.0.1:9999/'
      );
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '11.0.0.2:9999' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 2 http://11.0.0.2:9999/'
      );
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '15.0.0.2:80' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(await response.text(), 'hello binding: 3 http://15.0.0.2/');
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '[111::]:80' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(await response.text(), 'hello binding: 3 http://[111::]/');
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': 'google.com/hello/world' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 3 http://google.com/hello/world'
      );
    }

    // test we can set another TestService
    await container.interceptAllOutboundHttp(
      this.ctx.exports.TestService({ props: { id: 1212 } })
    );

    {
      // We preserved the order...
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '11.0.0.2:9999' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 2 http://11.0.0.2:9999/'
      );
    }

    {
      // and we updated the id, even for existing connections
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '15.0.0.2:80' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 1212 http://15.0.0.2/'
      );
    }

    {
      // and we updated the id for new connections
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '15.0.0.55:80' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 1212 http://15.0.0.55/'
      );
    }
  }

  async testInterceptWebSocket() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);
    // Set up egress mapping to route WebSocket requests to the binding
    await container.interceptOutboundHttp(
      '11.0.0.1:9999',
      this.ctx.exports.TestService({ props: { id: 42 } })
    );

    // Start container with WebSocket proxy mode enabled
    container.start({
      env: { WS_ENABLED: 'true', WS_PROXY_TARGET: '11.0.0.1:9999' },
    });

    container.monitor().catch((_err) => {});

    // Wait for container to be available
    await this.waitUntilContainerIsHealthy();

    assert.strictEqual(container.running, true);

    // Connect to container's /ws endpoint which proxies to the intercepted address
    // Flow: DO -> container:8080/ws -> container connects to 11.0.0.1:9999/ws
    //       -> sidecar intercepts -> workerd -> TestService worker binding
    const res = await container.getTcpPort(8080).fetch('http://foo/ws', {
      headers: {
        Upgrade: 'websocket',
        Connection: 'Upgrade',
        'Sec-WebSocket-Key': 'x3JJHMbDL1EzLkh9GBhXDw==',
        'Sec-WebSocket-Version': '13',
      },
      signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
    });

    // Should get WebSocket upgrade response
    assert.strictEqual(res.status, 101);
    assert.strictEqual(res.headers.get('upgrade'), 'websocket');
    assert.strictEqual(!!res.webSocket, true);

    const ws = res.webSocket;
    ws.binaryType = 'arraybuffer';
    ws.accept();

    // Listen for response
    const { promise, resolve, reject } = Promise.withResolvers();

    ws.addEventListener(
      'message',
      (event) => {
        resolve(event.data);
      },
      { once: true }
    );

    const timeout = setTimeout(() => {
      reject(new Error('Websocket message not received within 5 seconds'));
    }, 5_000);

    // Send a test message - should go through the whole chain and come back
    ws.send('Hello through intercept!');

    // Should receive response from TestService binding with id 42
    const response = new TextDecoder().decode(await promise);
    clearTimeout(timeout);
    assert.strictEqual(response, 'Binding 42: Hello through intercept!');

    ws.close();
    await container.destroy();
  }

  async testSnapshotRoundTrip() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const writeResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/test.txt', {
        method: 'POST',
        body: 'snapshot-content-123',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(writeResp.status, 200);

    const snapshot = await container.snapshotDirectory({ dir: '/app/data' });
    assert.strictEqual(typeof snapshot.id, 'string');
    assert.ok(snapshot.id.length > 0, 'snapshot id should be non-empty');
    assert.ok(snapshot.size > 0, 'snapshot size should be > 0');
    assert.strictEqual(snapshot.dir, '/app/data');
    assert.strictEqual(snapshot.name, undefined);

    await container.destroy();
    await monitor;
    assert.strictEqual(container.running, false);

    container.start({
      enableInternet: true,
      snapshots: [{ snapshot }],
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const readResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/test.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(readResp.status, 200);
    assert.strictEqual(await readResp.text(), 'snapshot-content-123');

    await container.destroy();
    await monitor2;
  }

  async testSnapshotNamedRoundTrip() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/named.txt', {
        method: 'POST',
        body: 'named-snapshot',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    const snapshot = await container.snapshotDirectory({
      dir: '/app/data',
      name: 'my-snapshot',
    });
    assert.strictEqual(snapshot.name, 'my-snapshot');
    assert.strictEqual(snapshot.dir, '/app/data');

    await container.destroy();
    await monitor;

    container.start({
      enableInternet: true,
      snapshots: [{ snapshot }],
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const readResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/named.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(readResp.status, 200);
    assert.strictEqual(await readResp.text(), 'named-snapshot');

    await container.destroy();
    await monitor2;
  }

  async testSnapshotMultipleDirectories() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/dir1/file1.txt', {
        method: 'POST',
        body: 'content-dir1',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/dir2/file2.txt', {
        method: 'POST',
        body: 'content-dir2',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    const snap1 = await container.snapshotDirectory({ dir: '/app/dir1' });
    const snap2 = await container.snapshotDirectory({ dir: '/app/dir2' });

    await container.destroy();
    await monitor;

    container.start({
      enableInternet: true,
      snapshots: [{ snapshot: snap1 }, { snapshot: snap2 }],
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const r1 = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/dir1/file1.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(r1.status, 200);
    assert.strictEqual(await r1.text(), 'content-dir1');

    const r2 = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/dir2/file2.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(r2.status, 200);
    assert.strictEqual(await r2.text(), 'content-dir2');

    await container.destroy();
    await monitor2;
  }

  async testSnapshotCustomMountPoint() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const writeResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/test.txt', {
        method: 'POST',
        body: 'remapped-content',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(writeResp.status, 200);

    const snapshot = await container.snapshotDirectory({ dir: '/app/data' });
    assert.strictEqual(snapshot.dir, '/app/data');

    await container.destroy();
    await monitor;

    container.start({
      enableInternet: true,
      snapshots: [{ snapshot, mountPoint: '/app/restored' }],
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const readResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/restored/test.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(readResp.status, 200);
    assert.strictEqual(await readResp.text(), 'remapped-content');

    // Must not exist at original path since we restored to a different mount point
    const origResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/test.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(origResp.status, 404);

    await container.destroy();
    await monitor2;
  }

  async testSnapshotRestoreToRoot() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const writeResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/root-test.txt', {
        method: 'POST',
        body: 'restore-to-root',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(writeResp.status, 200);

    const snapshot = await container.snapshotDirectory({ dir: '/app/data' });

    await container.destroy();
    await monitor;

    // Restoring to "/" places snapshot contents directly at the filesystem root
    container.start({
      enableInternet: true,
      snapshots: [{ snapshot, mountPoint: '/' }],
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const readResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/root-test.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(readResp.status, 200);
    assert.strictEqual(await readResp.text(), 'restore-to-root');

    await container.destroy();
    await monitor2;
  }

  async testSnapshotNonExistentDirectory() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await assert.rejects(
      () => container.snapshotDirectory({ dir: '/does/not/exist' }),
      (err) => {
        assert.match(err.message, /directory not found/);
        return true;
      }
    );

    await container.destroy();
    await monitor;
  }
}

export class TestService extends WorkerEntrypoint {
  fetch(request) {
    // Check if this is a WebSocket upgrade request
    const upgradeHeader = request.headers.get('Upgrade');
    if (upgradeHeader && upgradeHeader.toLowerCase() === 'websocket') {
      // Handle WebSocket upgrade
      const [client, server] = Object.values(new WebSocketPair());

      server.binaryType = 'arraybuffer';
      server.accept();

      server.addEventListener('message', (event) => {
        // Echo back with binding id prefix
        server.send(
          `Binding ${this.ctx.props.id}: ${new TextDecoder().decode(event.data)}`
        );
      });

      return new Response(null, {
        status: 101,
        webSocket: client,
      });
    }

    // Regular HTTP request
    return new Response(
      'hello binding: ' + this.ctx.props.id + ' ' + request.url
    );
  }
}

export class DurableObjectExample2 extends DurableObjectExample {}

// Test basic container status
export const testStatus = {
  async test(_ctrl, env) {
    for (const CONTAINER of [env.MY_CONTAINER, env.MY_DUPLICATE_CONTAINER]) {
      for (const name of ['testStatus', 'testStatus2']) {
        const id = CONTAINER.idFromName(getRandomDurableObjectName(name));
        const stub = CONTAINER.get(id);
        assert.strictEqual(await stub.getStatus(), false);
      }
    }
  },
};

// Test basic container functionality
export const testBasics = {
  async test(_ctrl, env) {
    for (const CONTAINER of [env.MY_CONTAINER, env.MY_DUPLICATE_CONTAINER]) {
      const id = CONTAINER.idFromName(getRandomDurableObjectName('testBasics'));
      const stub = CONTAINER.get(id);
      await stub.testBasics();
    }
  },
};

// Test exit code monitor functionality
export const testExitCode = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testExitCode')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testExitCode();
  },
};

// Test WebSocket functionality
export const testWebSockets = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testWebsockets')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testWs();
  },
};

export const testPortNotListening = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testPortNotListening')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testPortNotListening();
  },
};

// Test alarm functionality with containers
export const testAlarm = {
  async test(_ctrl, env) {
    // Test that we can recover the use_containers flag correctly in setAlarm
    // after a DO has been evicted
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testAlarm')
    );
    let stub = env.MY_CONTAINER.get(id);

    // Start immediate alarm
    await stub.startAlarm(true, 0);

    // Wait for alarm to trigger
    let retries = 0;
    while ((await stub.getAlarmIndex()) === 0 && retries < 50) {
      await scheduler.wait(20);
      retries++;
    }

    await scheduler.wait(50);

    // Set alarm for future and abort
    await stub.startAlarm(false, 1000);

    try {
      await stub.abort();
    } catch {
      // Expected to throw
    }

    stub = env.MY_CONTAINER.get(id);
    let confirmed = false;
    for (let i = 0; i < 50 && !confirmed; i++) {
      try {
        await stub.checkAlarmAbortConfirmation();
        confirmed = true;
      } catch (e) {
        assert.match(
          e.message,
          /Abort confirmation did not get inserted/,
          `Unexpected error while polling for alarm: ${e.message}`
        );

        await scheduler.wait(100);
      }
    }
    if (!confirmed) {
      await stub.checkAlarmAbortConfirmation();
    }
  },
};

export const testContainerShutdown = {
  async test(_, env) {
    const name = getRandomDurableObjectName('testContainerShutdown');

    {
      const stub = env.MY_CONTAINER.getByName(name);
      await stub.start();
      await assert.rejects(() => stub.abort(), {
        name: 'Error',
        message: 'Application called abort() to reset Durable Object.',
      });
    }

    // Wait for the container to be shutdown after the DO aborts
    await scheduler.wait(500);

    {
      const stub = env.MY_CONTAINER.getByName(name);

      // Container should not be running after DO exited
      await stub.expectRunning(false);
    }
  },
};

export const testSetInactivityTimeout = {
  async test(_ctrl, env) {
    const name = getRandomDurableObjectName('testSetInactivityTimeout');

    {
      const stub = env.MY_CONTAINER.getByName(name);

      await stub.testSetInactivityTimeout(10_000);

      await assert.rejects(() => stub.abort(), {
        name: 'Error',
        message: 'Application called abort() to reset Durable Object.',
      });
    }

    // Here we wait to ensure that if setInactivityTimeout *doesn't* work, the
    // container has enough time to shutdown after the DO is aborted. If we
    // don't wait then ctx.container.running will always be true, even without
    // setInactivityTimeout, because the container won't have stoped yet.
    await scheduler.wait(500);

    {
      const stub = env.MY_CONTAINER.getByName(name);

      // Container should still be running after DO exited
      await stub.expectRunning(true);
    }
  },
};

// Test PID namespace isolation behavior
// When containers_pid_namespace is ENABLED, the container has its own isolated PID namespace.
// We verify this by checking that PID 1 in the container's namespace is the container's
// init process, not the host's init process (systemd, launchd, etc.).
export const testPidNamespace = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testPidNamespace')
    );
    const stub = env.MY_CONTAINER.get(id);
    const data = await stub.testPidNamespace();

    // When using an isolated PID namespace, PID 1 should be the container's entrypoint
    // (a bash script that runs node), not the host's init process (systemd, launchd, init).
    assert.match(
      data.init,
      /container-client-test/,
      `Expected PID 1 to be the container entrypoint, but got: ${data.init}`
    );
  },
};

// Test setEgressHttp hostname functionality with internet (check we can establish
// outbound with others).
export const testSetEgressHttpWithInternet = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSetEgressHttpWithInternet')
    );
    let stub = env.MY_CONTAINER.get(id);
    await stub.testSetEgressHttpWithInternet();
  },
};

// Test setEgressHttp hostname functionality with no internet
export const testSetEgressHttpNoInternet = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSetEgressHttpNoInternet')
    );
    let stub = env.MY_CONTAINER.get(id);
    await stub.testSetEgressHttpNoInternet();
  },
};

// Test setEgressHttp functionality - registers a binding's channel token with the container
export const testSetEgressHttp = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSetEgressHttp')
    );
    let stub = env.MY_CONTAINER.get(id);
    await stub.testSetEgressHttp();
    try {
      // test we recover from aborts
      await stub.abort();
    } catch {}

    stub = env.MY_CONTAINER.get(id);
    // should work idempotent
    await stub.testSetEgressHttp();
  },
};

// Test WebSocket through interceptOutboundHttp - DO -> container -> worker binding via WebSocket
export const testInterceptWebSocket = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testInterceptWebSocket')
    );

    const stub = env.MY_CONTAINER.get(id);
    await stub.testInterceptWebSocket();
  },
};

// Test snapshot round-trip: write file -> snapshot -> destroy -> restore -> verify
export const testSnapshotRoundTrip = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotRoundTrip')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotRoundTrip();
  },
};

// Test snapshot with a human-friendly name
export const testSnapshotNamedRoundTrip = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotNamedRoundTrip')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotNamedRoundTrip();
  },
};

// Test snapshotting multiple directories and restoring them all
export const testSnapshotMultipleDirectories = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotMultipleDirectories')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotMultipleDirectories();
  },
};

// Test that snapshotting a non-existent directory gives a clear error
export const testSnapshotNonExistentDirectory = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotNonExistentDirectory')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotNonExistentDirectory();
  },
};

// Test restoring a snapshot to a different path than where it was captured
export const testSnapshotCustomMountPoint = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotCustomMountPoint')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotCustomMountPoint();
  },
};

// Test restoring a snapshot to / (root), exercising the restoreDir == "/" special case
export const testSnapshotRestoreToRoot = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotRestoreToRoot')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotRestoreToRoot();
  },
};

// Test that a snapshot created by one DO can be sent to another DO and restored there.
export const testSnapshotCrossDoRestore = {
  async test(_ctrl, env) {
    const sourceId = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotCrossDoRestore-source')
    );
    const targetId = env.MY_DUPLICATE_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotCrossDoRestore-target')
    );

    const source = env.MY_CONTAINER.get(sourceId);
    const target = env.MY_DUPLICATE_CONTAINER.get(targetId);

    const snapshot = await source.createSnapshotForTransfer();
    assert.strictEqual(snapshot.dir, '/app/data');
    assert.strictEqual(snapshot.name, 'cross-do-snapshot');

    await target.restoreTransferredSnapshot(snapshot);
  },
};
