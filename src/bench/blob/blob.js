// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Blob and File: construction, slicing, the attributes, structured cloning
// and FormData, which reads and makes them through api/blob-bridge.h. Each
// call does COUNT operations, since one costs about as much as the runner's
// overhead per call.

const COUNT = 16;

const kText = 'x'.repeat(64);
const kParts = Array.from({ length: 8 }, (_, i) => `part ${i} `);

export const construction = {
  bench(b) {
    b.run(`new Blob([string]) x${COUNT}`, () => {
      let blob;
      for (let i = 0; i < COUNT; i++) blob = new Blob([kText]);
      b.blackBox(blob);
    });
    b.run(`new Blob(8 strings, type) x${COUNT}`, () => {
      let blob;
      for (let i = 0; i < COUNT; i++) blob = new Blob(kParts, { type: 'a/b' });
      b.blackBox(blob);
    });
    const blob = new Blob([kText]);
    b.run(`new Blob([blob]) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = new Blob([blob]);
      b.blackBox(result);
    });
    b.run(`new File([string], name) x${COUNT}`, () => {
      let file;
      for (let i = 0; i < COUNT; i++) file = new File([kText], 'f.txt');
      b.blackBox(file);
    });
  },
};

export const slicing = {
  bench(b) {
    const blob = new Blob([kText], { type: 'a/b' });
    b.run(`slice(1, 32) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = blob.slice(1, 32);
      b.blackBox(result);
    });
    b.run(`slice(1, 32, type) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = blob.slice(1, 32, 'c/d');
      b.blackBox(result);
    });
  },
};

export const attributes = {
  bench(b) {
    const file = new File([kText], 'f.txt', { type: 'a/b' });
    b.run(`size + type + name x${COUNT}`, () => {
      let n = 0;
      for (let i = 0; i < COUNT; i++) {
        n += file.size + file.type.length + file.name.length;
      }
      b.blackBox(n);
    });
  },
};

export const serialization = {
  bench(b) {
    const blob = new Blob([kText], { type: 'a/b' });
    b.run(`structuredClone(blob) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = structuredClone(blob);
      b.blackBox(result);
    });
  },
};

export const formData = {
  bench(b) {
    const blob = new Blob([kText], { type: 'a/b' });
    const file = new File([kText], 'f.txt');
    b.run(`append(blob, name) + get() x${COUNT}`, () => {
      const form = new FormData();
      for (let i = 0; i < COUNT; i++) form.append('b', blob, 'b.txt');
      b.blackBox(form.getAll('b'));
    });
    b.run(`append(file) + get() x${COUNT}`, () => {
      const form = new FormData();
      for (let i = 0; i < COUNT; i++) form.append('f', file);
      b.blackBox(form.getAll('f'));
    });
  },
};
