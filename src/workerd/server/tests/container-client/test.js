import { DurableObject } from 'cloudflare:workers';
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
      const maxRetries = 6;
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

  async testSetInactivityTimeout() {
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

    await container.setInactivityTimeout(1000);
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

    // Wait for alarm to run after abort
    await scheduler.wait(1500);

    stub = env.MY_CONTAINER.get(id);
    await stub.checkAlarmAbortConfirmation();
  },
};

export const testSetInactivityTimeout = {
  async test(_ctrl, env) {
    {
      const stub = env.MY_CONTAINER.getByName('testSetInactivityTimeout');

      await stub.testSetInactivityTimeout();

      await assert.rejects(() => stub.abort(), {
        name: 'Error',
        message: 'Application called abort() to reset Durable Object.',
      });
    }

    {
      const stub = env.MY_CONTAINER.getByName('testSetInactivityTimeout');

      // Container should still be running after DO exited
      await stub.expectRunning(true);
    }
  },
};
