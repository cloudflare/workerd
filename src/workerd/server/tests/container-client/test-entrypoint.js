import { ContainerEntrypoint } from 'cloudflare:workers';
import assert from 'node:assert';
import { scheduler } from 'node:timers/promises';

// ContainerEntrypoint with only alarm-related methods for testing
export class ContainerEntrypointExample extends ContainerEntrypoint {
  defaultPort = 8080;
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
      await this.ctx.container.start();
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
}

export const testBasic = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName('testStart');
    const stub = env.MY_CONTAINER.get(id);

    await stub.start({
      envVars: { A: 'B', C: 'D' },
      entrypoint: ['node', 'app.js'],
      enableInternet: true,
      waitForReady: true,
    });
    let state = await stub.getState();
    assert.strictEqual(state.status, 'running');

    const resp = await stub.fetch('http://foo:8080/bar/baz', {
      method: 'POST',
      body: 'hello',
    });
    assert.equal(resp.status, 200);
    assert.strictEqual(await resp.text(), 'Hello World!');

    await stub.destroy();

    await scheduler.wait(3000);
    state = await stub.getState();
    assert.strictEqual(state.status, 'stopped');
  },
};

// Test WebSocket functionality via fetch
export const testWebSockets = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName('testWebsockets');
    const stub = env.MY_CONTAINER.get(id);

    // Start with WebSocket enabled
    await stub.start({
      entrypoint: ['node', 'app.js'],
      envVars: { WS_ENABLED: 'true' },
      enableInternet: true,
      waitForReady: true,
    });

    // Wait for ready
    await scheduler.wait(2000);

    // Test WebSocket upgrade via fetch
    const res = await stub.fetch('http://foo:8080/ws', {
      headers: {
        Upgrade: 'websocket',
        Connection: 'Upgrade',
        'Sec-WebSocket-Key': 'x3JJHMbDL1EzLkh9GBhXDw==',
        'Sec-WebSocket-Version': '13',
      },
    });

    assert.strictEqual(res.status, 101);
    assert.strictEqual(res.headers.get('upgrade'), 'websocket');
    assert.strictEqual(!!res.webSocket, true);

    const ws = res.webSocket;
    ws.accept();

    const messagePromise = new Promise((resolve) => {
      ws.addEventListener('message', (event) => resolve(event.data), {
        once: true,
      });
    });

    ws.send('Hello WebSocket!');
    assert.strictEqual(await messagePromise, 'Echo: Hello WebSocket!');

    ws.close();
    await stub.destroy();
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

    await stub.destroy();
  },
};
