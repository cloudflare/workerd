// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Bodies read at GLOBAL SCOPE, outside any IoContext, by every consumer. The
// reads start during module evaluation.

import { deepStrictEqual } from 'node:assert';

const kText = 'a=1&b=2';
const kFields = [
  ['a', '1'],
  ['b', '2'],
];
const kJson = '{"a":[1,2]}';

// A body of each kind, holding the bytes of `text`.
const sources = {
  string: (text) => text,
  Uint8Array: (text) => new TextEncoder().encode(text),
  ArrayBuffer: (text) => new TextEncoder().encode(text).buffer,
  Blob: (text) => new Blob([text]),
  URLSearchParams: (text) => new URLSearchParams(text),
  ReadableStream: (text) =>
    new ReadableStream({
      start(c) {
        c.enqueue(new TextEncoder().encode(text));
        c.close();
      },
    }),
};

// Each consumer, with the text of the body it reads and what it reads from it.
const decode = (bytes) => new TextDecoder().decode(bytes);
const consumers = {
  arrayBuffer: [kText, (body) => body.arrayBuffer().then(decode), kText],
  bytes: [kText, (body) => body.bytes().then(decode), kText],
  blob: [kText, (body) => body.blob().then((blob) => blob.size), kText.length],
  formData: [
    kText,
    (body) => body.formData().then((form) => [...form]),
    kFields,
  ],
  text: [kText, (body) => body.text(), kText],
  json: [kJson, (body) => body.json(), { a: [1, 2] }],
};

// Each read, settled, so that a failed read reaches the test rather than
// failing the module's evaluation.
const settle = (promise) =>
  promise.then(
    (value) => ({ value }),
    (error) => ({ error: String(error) })
  );

const reads = [];
for (const [kind, make] of Object.entries(sources)) {
  for (const [method, [text, read, expected]] of Object.entries(consumers)) {
    // URLSearchParams serializes as form data, which is not JSON.
    if (kind === 'URLSearchParams' && method === 'json') continue;
    const body = new Response(make(text), {
      headers: { 'content-type': 'application/x-www-form-urlencoded' },
    });
    reads.push({
      what: `${method}() of a ${kind} body`,
      result: settle(read(body)),
      expected,
    });
  }
}

// A FormData body is multipart, with a boundary of its own.
const form = new FormData();
for (const [name, value] of kFields) form.append(name, value);
reads.push({
  what: 'formData() of a FormData body',
  result: settle(new Response(form).formData().then((read) => [...read])),
  expected: kFields,
});

export const globalScopeBodyReads = {
  async test() {
    for (const { what, result, expected } of reads) {
      // The read's name is part of the value compared, so that a failure shows
      // it beside what the read produced.
      deepStrictEqual({ what, ...(await result) }, { what, value: expected });
    }
  },
};
