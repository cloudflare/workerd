import { strictEqual } from 'node:assert';

globalThis.fetch = async (url) => {
  const { default: data } = await import(url);
  return {
    async json() {
      return data;
    },
  };
};

globalThis.promise_test = (callback) => {
  callback();
};

globalThis.assert_equals = (a, b, c) => {
  strictEqual(a, b, c);
};

globalThis.test = (callback, message) => {
  try {
    callback();
  } catch (err) {
    const aerr = new AggregateError([err], message);
    globalThis.errors.push(aerr);
  }
};

globalThis.errors = [];
