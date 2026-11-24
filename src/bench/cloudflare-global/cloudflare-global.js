// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The Cloudflare global: the global itself, its flags, one flag, and the
// flags as a whole. A call does COUNT operations, since one costs about as
// much as the runner's overhead per call.

const COUNT = 16;

// The top level reads a flag, as feature detection does, so that the startup
// cases include making the object and its flags.
const kNodejsCompat = Cloudflare.compatibilityFlags.nodejs_compat;

export const reads = {
  bench(b) {
    b.blackBox(kNodejsCompat);
    b.run(`Cloudflare x${COUNT}`, () => {
      let value;
      for (let i = 0; i < COUNT; i++) value = Cloudflare;
      b.blackBox(value);
    });
    b.run(`Cloudflare.compatibilityFlags x${COUNT}`, () => {
      let value;
      for (let i = 0; i < COUNT; i++) value = Cloudflare.compatibilityFlags;
      b.blackBox(value);
    });
    b.run(`Cloudflare.compatibilityFlags.nodejs_compat x${COUNT}`, () => {
      let value;
      for (let i = 0; i < COUNT; i++) {
        value = Cloudflare.compatibilityFlags.nodejs_compat;
      }
      b.blackBox(value);
    });
    b.run(`Cloudflare.compatibilityFlags[name] x${COUNT}`, () => {
      const names = [
        'nodejs_compat',
        'url_standard',
        'not_a_flag',
        'streams_enable_constructors',
      ];
      let value;
      for (let i = 0; i < COUNT; i++) {
        value = Cloudflare.compatibilityFlags[names[i & 3]];
      }
      b.blackBox(value);
    });
  },
};

export const whole = {
  bench(b) {
    b.run('Object.keys(Cloudflare.compatibilityFlags)', () => {
      b.blackBox(Object.keys(Cloudflare.compatibilityFlags));
    });
    b.run('JSON.stringify(Cloudflare.compatibilityFlags)', () => {
      b.blackBox(JSON.stringify(Cloudflare.compatibilityFlags));
    });
    b.run(`Object.keys(Cloudflare) x${COUNT}`, () => {
      let keys;
      for (let i = 0; i < COUNT; i++) keys = Object.keys(Cloudflare);
      b.blackBox(keys);
    });
  },
};
