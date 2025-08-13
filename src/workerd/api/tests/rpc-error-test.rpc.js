export default {
  foo() {
    const err = new Error('boom');
    err.stack2 = err.stack;
    throw err;
  },

  async bar() {
    await scheduler.wait(10);
    return {
      async baz() {
        await scheduler.wait(10);
        const err = new Error('bork');
        err.stack2 = err.stack;
        throw err;
      },
    };
  },

  xyz() {
    const err = new Error('bang');
    err.stack2 = err.stack;
    return Promise.reject(err);
  },
};
