// More complex test case: WritableStreamDefaultWriter
// https://streams.spec.whatwg.org/#default-writer-class

[Exposed=*]
interface Foo {};

interface WritableStreamDefaultWriter {
  constructor(WritableStream stream);

  readonly attribute Promise<undefined> closed;
  readonly attribute unrestricted double? desiredSize;
  readonly attribute Promise<undefined> ready;

  Promise<undefined> abort(optional any reason);
  Promise<undefined> close();
  undefined releaseLock();
  Promise<undefined> write(optional any chunk);
};
