import { AsyncLocalStorage, AsyncResource } from 'node:async_hooks';

const als = new AsyncLocalStorage();

export default {
  async fetch(request) {
    const differentScope = als.run(123, () => AsyncResource.bind(() => {
      console.log(als.getStore());
    }));

    return als.run("Hello World", async () => {

      // differentScope is attached to a different async context, so
      // it will see a different value for als.getStore() (123)
      setTimeout(differentScope, 5);

      // Some simulated async delay.
      await scheduler.wait(10);
      return new Response(als.getStore());  // "Hello World"
    });
  }
};
