import { DurableObject } from 'cloudflare:workers';
import assert from 'node:assert';

const CONTAINER_NAME = 'container-example';
const IMAGE_TAG = 'ubuntu';

export class DurableObjectExample extends DurableObject {
  async testBasics() {
    let container = this.ctx.container;
    assert.strictEqual(container.running, false);

    // Test environment variable validation
    const badEnvs = [
      {
        value: { A: 'B\0' },
        err: "Environment variable values cannot contain '\\0': A",
      },
      {
        value: { 'B\0': 'A' },
        err: "Environment variable names cannot contain '\\0': B\0",
      },
      {
        value: { 'B=': 'A' },
        err: "Environment variable names cannot contain '=': B=",
      },
    ];

    for (const badEnv of badEnvs) {
      await assert.rejects(
        async () => {
          await container.start({
            entrypoint: ['/bin/sh'],
            env: badEnv.value,
            enableInternet: true,
          });
          throw new Error("start should've thrown an error");
        },
        {
          message: badEnv.err,
        }
      );
    }

    // Start container with valid configuration
    await container.start({
      entrypoint: ['/bin/sh'],
      env: { A: 'B', C: 'D', L: 'F' },
      enableInternet: true,
    });

    // let monitor = container.monitor().catch((err) => this.ctx.abort(err));

    // TODO: Enable tests when getTcpPort() is fully implemented.
    // Test simple TCP connection on port 20
    // {
    //   let sock = container.getTcpPort(20).connect('foo:123');
    //   let reader = sock.readable.getReader();
    //   let { done, value } = await reader.read();
    //   assert.strictEqual(
    //     new TextDecoder().decode(value),
    //     "Hello, you've reached port 20"
    //   );
    //   await sock.close();
    // }

    // Test container information retrieval on port 123
    // {
    //   let sock = container.getTcpPort(123).connect('foo:123');
    //   let reader = sock.readable.getReader();
    //   let { done, value } = await reader.read();
    //   let response = new TextDecoder().decode(value);

    //   // Check that response contains expected container information
    //   assert(response.includes('entrypoint: foo, bar, baz'));
    //   assert(response.includes('environment variables: A=B, C=D, L=F'));
    //   assert(response.includes('enable internet: true'));
    //   await sock.close();
    // }

    // Test HTTP requests to container
    // {
    //   let resp = await container
    //     .getTcpPort(80)
    //     .fetch('http://foo/bar/baz', { method: 'POST', body: 'hello' });
    //
    //   assert.equal(resp.status, 234);
    //   assert.equal(resp.statusText, 'Fake OK');
    //   assert.equal(resp.headers.get('Content-Type'), 'text/foobar');
    //
    //   let text = await resp.text();
    //   assert(text.includes('Hello from HTTP! You said:'));
    //   assert(text.includes('method: POST'));
    //   assert(text.includes('host: foo'));
    //   assert(text.includes('path: /bar/baz'));
    //   assert(text.includes('body: hello'));
    // }
    await container.destroy();
    // await monitor;
    assert.strictEqual(container.running, false);
  }

  async leaveRunning() {
    // Start container and leave it running
    let container = this.ctx.container;
    if (!container.running) {
      await container.start({
        entrypoint: ['leave-running'],
      });
    }

    assert.strictEqual(container.running, true);
  }

  async checkRunning() {
    // Check container was started using leaveRunning()
    const container = this.ctx.container;

    // Let's guard in case the test assumptions are wrong.
    if (!container.running) {
      return;
    }

    // TODO: Enable these tests once getTcpPort() is working.
    // {
    //   let sock = container.getTcpPort(123).connect('foo:123');
    //   let reader = sock.readable.getReader();
    //   let { done, value } = await reader.read();
    //   let response = new TextDecoder().decode(value);

    //   assert(response.includes('entrypoint: leave-running'));
    //   assert(response.includes('environment variables: '));
    //   assert(response.includes('enable internet: false'));
    //   await sock.close();
    // }

    await container.destroy();
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
      await this.ctx.container.start();
    } else if (start) {
      // This is potentially bug with the ordering of your tests.
      // This function is called assuming the container is not running,
      // but previous test probably left the container open.
      console.warn('WARNING: Start called but container is already running');
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
    await this.ctx.container.start();
    // Test WebSocket upgrade request
    const res = await this.ctx.container.getTcpPort(80).fetch('http://foo/ws', {
      headers: { Upgrade: 'websocket' },
    });
    assert.strictEqual(res.status, 400);
    assert.strictEqual(!!res.webSocket, false);
  }

  getStatus() {
    return this.ctx.container.running;
  }
}

// Test basic container status
export const testStatus = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(CONTAINER_NAME);
    assert.strictEqual(id.name, CONTAINER_NAME);
    const container = env.MY_CONTAINER.get(id);
    assert.strictEqual(await container.getStatus(), false);
  },
};

// Test basic container functionality
export const testBasics = {
  async test(_ctrl, env) {
    let id = env.MY_CONTAINER.idFromName('testBasics');
    let stub = env.MY_CONTAINER.get(id);
    await stub.testBasics();
  },
};

// Test container persistence across durable object instances
export const testAlreadyRunning = {
  async test(_ctrl, env) {
    let id = env.MY_CONTAINER.idFromName('testAlreadyRunning');
    let stub = env.MY_CONTAINER.get(id);

    await stub.leaveRunning();

    try {
      await stub.abort();
      throw new Error('Expected abort to throw');
    } catch (err) {
      assert.strictEqual(err.name, 'Error');
      assert.strictEqual(
        err.message,
        'Application called abort() to reset Durable Object.'
      );
    }

    // Recreate stub to get a new instance
    stub = env.MY_CONTAINER.get(id);
    await stub.checkRunning();
  },
};

// Test WebSocket functionality
export const testWebSockets = {
  async test(_ctrl, env) {
    let id = env.MY_CONTAINER.idFromName('testWebsockets');
    let stub = env.MY_CONTAINER.get(id);
    // await stub.testWs();
  },
};

// Test alarm functionality with containers
export const testAlarm = {
  async test(_ctrl, env) {
    // Test that we can recover the use_containers flag correctly in setAlarm
    // after a DO has been evicted
    let id = env.MY_CONTAINER.idFromName('testAlarm');
    let stub = env.MY_CONTAINER.get(id);

    // Start immediate alarm
    await stub.startAlarm(true, 0);

    // Wait for alarm to trigger
    let retries = 0;
    while ((await stub.getAlarmIndex()) === 0 && retries < 50) {
      await scheduler.wait(20);
      retries++;
    }

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
