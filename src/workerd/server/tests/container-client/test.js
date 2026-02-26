import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';
import assert from 'node:assert';
import { scheduler } from 'node:timers/promises';

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

    // Test HTTP requests to container
    {
      let resp;
      // The retry count here is arbitrary. Can increase it if necessary.
      const maxRetries = 15;
      for (let i = 1; i <= maxRetries; i++) {
        try {
          resp = await container
            .getTcpPort(8080)
            .fetch('http://foo/bar/baz', { method: 'POST', body: 'hello' });
          break;
        } catch (e) {
          if (!e.message.includes('container port not found')) {
            throw e;
          }
          console.info(
            `Retrying getTcpPort(8080) for the ${i} time due to an error ${e.message}`
          );
          console.info(e);
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
    // assert.strictEqual(this.ctx.container.running, true);
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

    // Wait for container to be ready with retry loop
    let res;
    // The retry count here is arbitrary. Can increase it if necessary.
    const maxRetries = 6;
    for (let i = 1; i <= maxRetries; i++) {
      try {
        res = await container.getTcpPort(8080).fetch('http://foo/ws', {
          headers: {
            Upgrade: 'websocket',
            Connection: 'Upgrade',
            'Sec-WebSocket-Key': 'x3JJHMbDL1EzLkh9GBhXDw==',
            'Sec-WebSocket-Version': '13',
          },
        });
        break;
      } catch (e) {
        if (!e.message.includes('container port not found')) {
          throw e;
        }
        console.info(
          `Retrying getTcpPort(8080) for the ${i} time due to an error ${e.message}`
        );
        console.info(e);
        if (i === maxRetries) {
          console.error(
            `Failed to connect to container for WebSocket ${container.id}. Retried ${i} times`
          );
          throw e;
        }
        await scheduler.wait(1000);
      }
    }

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

  async ping() {
    const container = this.ctx.container;
    {
      let resp;
      // The retry count here is arbitrary. Can increase it if necessary.
      const maxRetries = 15;
      for (let i = 1; i <= maxRetries; i++) {
        try {
          resp = await container
            .getTcpPort(8080)
            .fetch('http://foo/bar/baz', { method: 'POST', body: 'hello' });
          break;
        } catch (e) {
          if (!e.message.includes('container port not found')) {
            throw e;
          }
          console.info(
            `Retrying getTcpPort(8080) for the ${i} time due to an error ${e.message}`
          );
          console.info(e);
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
    // Fetch the /pid-namespace endpoint which returns info about the PID namespace
    let resp;
    const maxRetries = 6;
    for (let i = 1; i <= maxRetries; i++) {
      try {
        resp = await container
          .getTcpPort(8080)
          .fetch('http://foo/pid-namespace');
        break;
      } catch (e) {
        if (!e.message.includes('container port not found')) {
          throw e;
        }
        console.info(
          `Retrying getTcpPort(8080) for the ${i} time due to an error ${e.message}`
        );
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
    const data = await resp.json();

    await container.destroy();
    await monitor;
    assert.strictEqual(container.running, false);

    return data;
  }

  async testSetEgressHttp() {
    const container = this.ctx.container;

    // We do not assert the container is running or not, this method
    // should work as-is

    // Set up egress TCP mapping to route requests to the binding
    // We can configure this even before the container starts.
    await container.interceptOutboundHttp(
      '1.2.3.4',
      this.ctx.exports.TestService({ props: { id: 1234 } })
    );

    if (!container.running) {
      container.start();
    }

    // wait for container to be available
    await this.ping();

    assert.strictEqual(container.running, true);

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

    // we catch all HTTP requests to port 80
    await container.interceptAllOutboundHttp(
      this.ctx.exports.TestService({ props: { id: 3 } })
    );

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '1.2.3.4:80' },
        });
      assert.equal(response.status, 200);
      assert.equal(await response.text(), 'hello binding: 1234');
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '11.0.0.1:9999' },
        });
      assert.equal(response.status, 200);
      assert.equal(await response.text(), 'hello binding: 1');
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '11.0.0.2:9999' },
        });
      assert.equal(response.status, 200);
      assert.equal(await response.text(), 'hello binding: 2');
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '15.0.0.2:80' },
        });
      assert.equal(response.status, 200);
      assert.equal(await response.text(), 'hello binding: 3');
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '[111::]:80' },
        });
      assert.equal(response.status, 200);
      assert.equal(await response.text(), 'hello binding: 3');
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

    // Start container with WebSocket proxy mode enabled
    container.start({
      env: { WS_ENABLED: 'true', WS_PROXY_TARGET: '11.0.0.1:9999' },
    });

    // Wait for container to be available
    await this.ping();

    assert.strictEqual(container.running, true);

    // Set up egress mapping to route WebSocket requests to the binding
    await container.interceptOutboundHttp(
      '11.0.0.1:9999',
      this.ctx.exports.TestService({ props: { id: 42 } })
    );

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
    });

    // Should get WebSocket upgrade response
    assert.strictEqual(res.status, 101);
    assert.strictEqual(res.headers.get('upgrade'), 'websocket');
    assert.strictEqual(!!res.webSocket, true);

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

    // Send a test message - should go through the whole chain and come back
    ws.send('Hello through intercept!');

    // Should receive response from TestService binding with id 42
    const response = new TextDecoder().decode(await messagePromise);
    assert.strictEqual(response, 'Binding 42: Hello through intercept!');

    ws.close();
    await container.destroy();
  }
}

export class TestService extends WorkerEntrypoint {
  fetch(request) {
    // Check if this is a WebSocket upgrade request
    const upgradeHeader = request.headers.get('Upgrade');
    if (upgradeHeader && upgradeHeader.toLowerCase() === 'websocket') {
      // Handle WebSocket upgrade
      const [client, server] = Object.values(new WebSocketPair());

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
    return new Response('hello binding: ' + this.ctx.props.id);
  }
}

export class DurableObjectExample2 extends DurableObjectExample {}

// Test basic container status
export const testStatus = {
  async test(_ctrl, env) {
    for (const CONTAINER of [env.MY_CONTAINER, env.MY_DUPLICATE_CONTAINER]) {
      for (const name of ['testStatus', 'testStatus2']) {
        const id = CONTAINER.idFromName(name);
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
      const id = CONTAINER.idFromName('testBasics');
      const stub = CONTAINER.get(id);
      await stub.testBasics();
    }
  },
};

// Test exit code monitor functionality
export const testExitCode = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName('testExitCode');
    const stub = env.MY_CONTAINER.get(id);
    await stub.testExitCode();
  },
};

// Test WebSocket functionality
export const testWebSockets = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName('testWebsockets');
    const stub = env.MY_CONTAINER.get(id);
    await stub.testWs();
  },
};

// Test alarm functionality with containers
export const testAlarm = {
  async test(_ctrl, env) {
    // Test that we can recover the use_containers flag correctly in setAlarm
    // after a DO has been evicted
    const id = env.MY_CONTAINER.idFromName('testAlarm');
    let stub = env.MY_CONTAINER.get(id);

    // Start immediate alarm
    await stub.startAlarm(true, 0);

    // Wait for alarm to trigger
    let retries = 0;
    while ((await stub.getAlarmIndex()) === 0 && retries < 50) {
      await scheduler.wait(20);
      retries++;
    }

    // Wait for container to start
    await scheduler.wait(500);

    // Set alarm for future and abort
    await stub.startAlarm(false, 1000);

    try {
      await stub.abort();
    } catch {
      // Expected to throw
    }

    // Poll for the alarm to run after abort. The DO must be re-created and the
    // alarm handler must fire, which can take variable time on CI.
    // 50 iterations * 200ms = 10s max wait, which gives plenty of headroom for
    // slow CI environments where Docker and DO reconstruction add latency.
    stub = env.MY_CONTAINER.get(id);
    let confirmed = false;
    for (let i = 0; i < 50 && !confirmed; i++) {
      await scheduler.wait(200);
      try {
        await stub.checkAlarmAbortConfirmation();
        confirmed = true;
      } catch (e) {
        assert.match(
          e.message,
          /Abort confirmation did not get inserted/,
          `Unexpected error while polling for alarm: ${e.message}`
        );
      }
    }
    if (!confirmed) {
      await stub.checkAlarmAbortConfirmation();
    }
  },
};

export const testContainerShutdown = {
  async test(_, env) {
    {
      const stub = env.MY_CONTAINER.getByName('testContainerShutdown');
      await stub.start();
      await assert.rejects(() => stub.abort(), {
        name: 'Error',
        message: 'Application called abort() to reset Durable Object.',
      });
    }

    // Wait for the container to be shutdown after the DO aborts
    await scheduler.wait(500);

    {
      const stub = env.MY_CONTAINER.getByName('testContainerShutdown');

      // Container should not be running after DO exited
      await stub.expectRunning(false);
    }
  },
};

export const testSetInactivityTimeout = {
  async test(_ctrl, env) {
    {
      const stub = env.MY_CONTAINER.getByName('testSetInactivityTimeout');

      await stub.testSetInactivityTimeout(3000);

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
      const stub = env.MY_CONTAINER.getByName('testSetInactivityTimeout');

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
    const id = env.MY_CONTAINER.idFromName('testPidNamespace');
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

// Test setEgressHttp functionality - registers a binding's channel token with the container
export const testSetEgressHttp = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName('testSetEgressHttp');
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
    const id = env.MY_CONTAINER.idFromName('testInterceptWebSocket');
    const stub = env.MY_CONTAINER.get(id);
    await stub.testInterceptWebSocket();
  },
};
