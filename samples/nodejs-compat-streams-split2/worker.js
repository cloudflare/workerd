import {
  PassThrough,
  Transform,
  Readable,
} from 'node:stream';

import { default as split2 } from 'split2';

const enc = new TextEncoder();

export default {
  async fetch() {
    const pt = new PassThrough();

    // split2 will remove the new lines from the single input stream,
    // pushing each individual line as a separate chunk. We use this
    // transform to add the new lines back in just to show the effect
    // in the output.
    const lb = new Transform({
      transform(chunk, encoding, callback) {
        callback(null, enc.encode(chunk + '\n'));
      }
    });

    const readable = pt.pipe(split2()).pipe(lb);

    pt.end('hello\nfrom\nthe\nwonderful\nworld\nof\nnode.js\nstreams!');

    return new Response(Readable.toWeb(readable));
  }
};
