import {
  Readable,
  Transform,
} from 'node:stream';

import {
  text,
} from 'node:stream/consumers';

import {
  pipeline,
} from 'node:stream/promises';

class MyTransform extends Transform {
  constructor() {
    super({ encoding: 'utf8' });
  }
  _transform(chunk, _, cb) {
    this.push(chunk.toString().toUpperCase());
    cb();
  }
  _flush(cb) {
    this.push('\n');
    cb();
  }
}

export default {
  async fetch() {
    const chunks = [
      "hello ",
      "from ",
      "the ",
      "wonderful ",
      "world ",
      "of ",
      "node.js ",
      "streams!"
    ];

    function nextChunk(readable) {
      readable.push(chunks.shift());
      if (chunks.length === 0) readable.push(null);
      else queueMicrotask(() => nextChunk(readable));
    }

    const readable = new Readable({
      encoding: 'utf8',
      read() { nextChunk(readable); }
    });

    const transform = new MyTransform();

    await pipeline(readable, transform);

    return new Response(await text(transform));
  }
};
