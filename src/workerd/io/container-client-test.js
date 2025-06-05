import { DurableObject } from 'cloudflare:workers';
import assert from 'node:assert';

const OPEN_CONTAINER_PORT = 5000;
const CONTAINER_NAME = 'container-example';

async function startAndWaitForPort(container, portToAwait, maxTries = 10) {
  const port = container.getTcpPort(portToAwait);
  // promise to make sure the container does not exit
  let monitor;

  function onContainerExit() {
    console.log('Container exited');
  }

  // the "err" value can be customized by the destroy() method
  async function onContainerError(err) {
    console.log('Container errored', err);
  }

  for (let i = 0; i < maxTries; i++) {
    try {
      console.log(`Waiting for container to start ${i + 1}/${maxTries}`);
      if (!container.running) {
        container.start();
        // force DO to keep track of running state
        monitor = container
          .monitor()
          .then(onContainerExit)
          .catch(onContainerError);
      }

      await (await port.fetch('http://ping')).text();
      return;
    } catch (err) {
      console.error('Error connecting to the container on', i, 'try', err);

      if (err.message.includes('listening')) {
        await new Promise((res) => setTimeout(res, 300));
        continue;
      }

      // no container yet
      if (
        err.message.includes(
          'there is no container instance that can be provided'
        )
      ) {
        await new Promise((res) => setTimeout(res, 300));
        continue;
      }

      throw err;
    }
  }

  throw new Error(
    `could not check container healthiness after ${maxTries} tries`
  );
}

export class DurableObjectExample extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    // ctx.blockConcurrencyWhile(async () => {
    //   await startAndWaitForPort(ctx.container, OPEN_CONTAINER_PORT);
    // });
  }

  async fetch(request) {
    return await this.ctx.container
      .getTcpPort(OPEN_CONTAINER_PORT)
      .fetch(request.url.replace('https://', 'http://'), request.clone());
  }

  async destroySelf() {
    await this.ctx.container.destroy('Manually Destroyed');
  }

  async signal(signalString) {
    let signalNumber = parseInt(signalString, 10);
    this.ctx.container.signal(signalNumber);
  }

  getStatus() {
    return this.ctx.container.running;
  }
}

export const testStatus = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(CONTAINER_NAME);
    assert.strictEqual(id.name, 'container-example');
    const container = env.MY_CONTAINER.get(id);
    assert.strictEqual(await container.getStatus(), false);
    console.log('succeeded');
  },
};
