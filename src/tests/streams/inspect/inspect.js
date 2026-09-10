// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// util.inspect output for every stream surface. DIVERGENCE, wholesale:
// the C++ implementation installs a custom inspect exposing lock/state
// internals ([state], [supportsBYOB], [length], [expectsBytes]); the
// TypeScript implementation has none — every stream inspects as a bare
// "ClassName {}" regardless of state. Each C++ string is pinned
// verbatim (migrated from api/streams/streams-test.js); the TS side
// pins the bare form at every transition point so the gap stays
// visible.

import { strictEqual } from 'node:assert';
import util from 'node:util';
import { usingTsImpl } from 'which-impl';

// Returns an assert bound to inspect options: the C++ string, or the
// bare "Class {}" under TS.
function checker(opts) {
  return (value, cppExpected, bare) =>
    strictEqual(util.inspect(value, opts), usingTsImpl ? bare : cppExpected);
}

export const inspectValueReadable = {
  async test() {
    const expect = checker({ breakLength: Infinity });
    let pulls = 0;
    const rs = new ReadableStream({
      pull(controller) {
        if (pulls === 0) controller.enqueue('hello');
        if (pulls === 1) controller.close();
        pulls++;
      },
    });
    const bare = 'ReadableStream {}';
    expect(
      rs,
      "ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: false, [length]: undefined }",
      bare
    );
    const reader = rs.getReader();
    expect(
      rs,
      "ReadableStream { locked: true, [state]: 'readable', [supportsBYOB]: false, [length]: undefined }",
      bare
    );
    await reader.read();
    expect(
      rs,
      "ReadableStream { locked: true, [state]: 'readable', [supportsBYOB]: false, [length]: undefined }",
      bare
    );
    await reader.read();
    expect(
      rs,
      "ReadableStream { locked: true, [state]: 'closed', [supportsBYOB]: false, [length]: undefined }",
      bare
    );
  },
};

export const inspectErroredReadable = {
  test() {
    const expect = checker({ breakLength: Infinity });
    const rs = new ReadableStream({
      start(controller) {
        controller.error(new Error('Oops!'));
      },
    });
    expect(
      rs,
      "ReadableStream { locked: false, [state]: 'errored', [supportsBYOB]: false, [length]: undefined }",
      'ReadableStream {}'
    );
  },
};

export const inspectByteReadable = {
  test() {
    const expect = checker({ breakLength: Infinity });
    const rs = new ReadableStream({
      type: 'bytes',
      pull(controller) {
        controller.enqueue(new Uint8Array([1]));
      },
    });
    expect(
      rs,
      "ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: undefined }",
      'ReadableStream {}'
    );
  },
};

export const inspectWritable = {
  async test() {
    const expect = checker({ breakLength: Infinity });
    const ws = new WritableStream({ write() {} });
    const bare = 'WritableStream {}';
    expect(
      ws,
      "WritableStream { locked: false, [state]: 'writable', [expectsBytes]: false }",
      bare
    );
    const writer = ws.getWriter();
    expect(
      ws,
      "WritableStream { locked: true, [state]: 'writable', [expectsBytes]: false }",
      bare
    );
    await writer.write('chunk');
    expect(
      ws,
      "WritableStream { locked: true, [state]: 'writable', [expectsBytes]: false }",
      bare
    );
    await writer.close();
    expect(
      ws,
      "WritableStream { locked: true, [state]: 'closed', [expectsBytes]: false }",
      bare
    );
  },
};

export const inspectErroringWritable = {
  async test() {
    const expect = checker({ breakLength: Infinity });
    const ws = new WritableStream({
      write(chunk, controller) {
        controller.error(new Error('Oops!'));
      },
    });
    const bare = 'WritableStream {}';
    expect(
      ws,
      "WritableStream { locked: false, [state]: 'writable', [expectsBytes]: false }",
      bare
    );
    const writer = ws.getWriter();
    const promise = writer.write('chunk');
    expect(
      ws,
      "WritableStream { locked: true, [state]: 'erroring', [expectsBytes]: false }",
      bare
    );
    await promise;
    expect(
      ws,
      "WritableStream { locked: true, [state]: 'errored', [expectsBytes]: false }",
      bare
    );
  },
};

export const inspectFixedLengthStream = {
  async test() {
    const expect = checker({ breakLength: 100 });
    const fls = new FixedLengthStream(5);
    const bare = 'FixedLengthStream {}';
    expect(
      fls,
      `FixedLengthStream {
  readable: ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: 5n },
  writable: WritableStream { locked: false, [state]: 'writable', [expectsBytes]: true }
}`,
      bare
    );
    const { writable, readable } = fls;
    const writer = writable.getWriter();
    expect(
      fls,
      `FixedLengthStream {
  readable: ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: 5n },
  writable: WritableStream { locked: true, [state]: 'writable', [expectsBytes]: true }
}`,
      bare
    );
    void writer.write(new Uint8Array([1, 2, 3]));
    void writer.write(new Uint8Array([4, 5]));
    void writer.close();
    expect(
      fls,
      `FixedLengthStream {
  readable: ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: 5n },
  writable: WritableStream { locked: true, [state]: 'closed', [expectsBytes]: true }
}`,
      bare
    );
    const reader = readable.getReader();
    await reader.read();
    expect(
      fls,
      `FixedLengthStream {
  readable: ReadableStream { locked: true, [state]: 'readable', [supportsBYOB]: true, [length]: 2n },
  writable: WritableStream { locked: true, [state]: 'closed', [expectsBytes]: true }
}`,
      bare
    );
    await reader.read();
    expect(
      fls,
      `FixedLengthStream {
  readable: ReadableStream { locked: true, [state]: 'readable', [supportsBYOB]: true, [length]: 0n },
  writable: WritableStream { locked: true, [state]: 'closed', [expectsBytes]: true }
}`,
      bare
    );
    await reader.read();
    expect(
      fls,
      `FixedLengthStream {
  readable: ReadableStream { locked: true, [state]: 'closed', [supportsBYOB]: true, [length]: 0n },
  writable: WritableStream { locked: true, [state]: 'closed', [expectsBytes]: true }
}`,
      bare
    );
  },
};

export const inspectErroredIdentityStream = {
  async test() {
    const expect = checker({ breakLength: 100 });
    const its = new IdentityTransformStream();
    const bare = 'IdentityTransformStream {}';
    expect(
      its,
      `IdentityTransformStream {
  readable: ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: undefined },
  writable: WritableStream { locked: false, [state]: 'writable', [expectsBytes]: true }
}`,
      bare
    );
    const { writable, readable } = its;
    const writer = writable.getWriter();
    void writer.abort(new Error('Oops!'));
    expect(
      its,
      `IdentityTransformStream {
  readable: ReadableStream { locked: false, [state]: 'readable', [supportsBYOB]: true, [length]: undefined },
  writable: WritableStream { locked: true, [state]: 'errored', [expectsBytes]: true }
}`,
      bare
    );
    const reader = readable.getReader();
    expect(
      its,
      `IdentityTransformStream {
  readable: ReadableStream { locked: true, [state]: 'readable', [supportsBYOB]: true, [length]: undefined },
  writable: WritableStream { locked: true, [state]: 'errored', [expectsBytes]: true }
}`,
      bare
    );
    await reader.read().catch(() => {});
    expect(
      its,
      `IdentityTransformStream {
  readable: ReadableStream { locked: true, [state]: 'errored', [supportsBYOB]: true, [length]: undefined },
  writable: WritableStream { locked: true, [state]: 'errored', [expectsBytes]: true }
}`,
      bare
    );
  },
};
