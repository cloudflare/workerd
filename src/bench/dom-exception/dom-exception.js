// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// DOMException: constructed by script and by native code, its attributes
// and stack read, and structured cloning. Each call does COUNT operations,
// since one costs about as much as the runner's overhead per call.

const COUNT = 16;

export const construction = {
  bench(b) {
    b.run(`new DOMException(message, name) x${COUNT}`, () => {
      let error;
      for (let i = 0; i < COUNT; i++) {
        error = new DOMException('The operation was aborted', 'AbortError');
      }
      b.blackBox(error);
    });
    b.run(`new DOMException() .stack x${COUNT}`, () => {
      let stack;
      for (let i = 0; i < COUNT; i++) {
        stack = new DOMException('m', 'NotFoundError').stack;
      }
      b.blackBox(stack);
    });
    b.run(`throw and catch x${COUNT}`, () => {
      let caught;
      for (let i = 0; i < COUNT; i++) {
        try {
          throw new DOMException('m', 'InvalidStateError');
        } catch (error) {
          caught = error;
        }
      }
      b.blackBox(caught);
    });
  },
};

export const nativeCreation = {
  bench(b) {
    // A tunneled JSG error.
    b.run(`atob() error x${COUNT}`, () => {
      let caught;
      for (let i = 0; i < COUNT; i++) {
        try {
          atob('!');
        } catch (error) {
          caught = error;
        }
      }
      b.blackBox(caught);
    });
    // Lock::domException() directly.
    b.run(`AbortSignal.abort().reason x${COUNT}`, () => {
      let reason;
      for (let i = 0; i < COUNT; i++) reason = AbortSignal.abort().reason;
      b.blackBox(reason);
    });
  },
};

export const attributes = {
  bench(b) {
    const error = new DOMException('m', 'AbortError');
    b.run(`.name .message .code x${COUNT}`, () => {
      let sum = 0;
      for (let i = 0; i < COUNT; i++) {
        sum += error.name.length + error.message.length + error.code;
      }
      b.blackBox(sum);
    });
  },
};

export const cloning = {
  bench(b) {
    const error = new DOMException('m', 'AbortError');
    b.run(`structuredClone(DOMException) x${COUNT}`, () => {
      let clone;
      for (let i = 0; i < COUNT; i++) clone = structuredClone(error);
      b.blackBox(clone);
    });
  },
};
