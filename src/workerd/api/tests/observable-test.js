import {
  strictEqual,
  deepStrictEqual,
} from 'node:assert';

export const testObservable = {
  async test() {
    const et = new EventTarget();
    const p = et.on('test').take(2).toArray();
    et.dispatchEvent(new Event('test'));
    et.dispatchEvent(new Event('test'));
    et.dispatchEvent(new Event('test'));
    const r = await p;
    strictEqual(r.length, 2);
  }
};

export const testObservableFromObservable = {
  async test() {
    const ob1 = new Observable(() => {});
    const ob2 = Observable.from(ob1);
    strictEqual(ob1, ob2);
  }
};

export const testObservableFromPromise = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const ob1 = Observable.from(promise);
    const p = ob1.first();
    resolve(42);
    strictEqual(await p, 42);
  }
};

export const testObservableFromIterable = {
  async test() {
    const ob1 = Observable.from([1, 2, 3]);
    const p = ob1.toArray();
    deepStrictEqual(await p, [1, 2, 3]);
  }
};

export const testObservableFromAsyncPromise = {
  async test() {
    const gen = async function*() {
      yield 1;
      yield 2;
      yield 3;
    };
    const ob1 = Observable.from(gen());
    const p = ob1.toArray();
    deepStrictEqual(await p, [1, 2, 3]);
  }
};

export const testObservableFromAsyncIterableWithAbort = {
  async test() {
    const ac = new AbortController();
    const gen = async function*() {
      yield 1;
      ac.abort();
      yield 2;
    };
    const ob1 = Observable.from(gen());
    const p = ob1.toArray({ signal: ac.signal });
    try {
      await p;
      throw new Error('should have thrown');
    } catch (err) {
      strictEqual(err.name, 'AbortError');
    }
  }
};
