// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The navigator: the global itself, each attribute, the instance as a
// whole, and sendBeacon(). A call does COUNT operations, since one costs
// about as much as the runner's overhead per call.

const COUNT = 16;

// The top level reads the navigator, as feature detection commonly does, so
// that the startup cases include making it.
const kUserAgent = navigator.userAgent;

function attribute(b, name) {
  b.run(`navigator.${name} x${COUNT}`, () => {
    let value;
    for (let i = 0; i < COUNT; i++) value = navigator[name];
    b.blackBox(value);
  });
}

export const attributes = {
  bench(b) {
    b.blackBox(kUserAgent);
    b.run(`navigator x${COUNT}`, () => {
      let value;
      for (let i = 0; i < COUNT; i++) value = navigator;
      b.blackBox(value);
    });
    attribute(b, 'userAgent');
    attribute(b, 'hardwareConcurrency');
    attribute(b, 'platform');
    attribute(b, 'language');
    attribute(b, 'languages');
    attribute(b, 'storage');
    b.run(`navigator.languages[0] x${COUNT}`, () => {
      let value;
      for (let i = 0; i < COUNT; i++) value = navigator.languages[0];
      b.blackBox(value);
    });
  },
};

export const instance = {
  bench(b) {
    b.run(`Object.keys(navigator) x${COUNT}`, () => {
      let keys;
      for (let i = 0; i < COUNT; i++) keys = Object.keys(navigator);
      b.blackBox(keys);
    });
    b.run(`JSON.stringify(navigator) x${COUNT}`, () => {
      let json;
      for (let i = 0; i < COUNT; i++) json = JSON.stringify(navigator);
      b.blackBox(json);
    });
    b.run(`navigator instanceof Navigator x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = navigator instanceof Navigator;
      b.blackBox(result);
    });
  },
};

export const sendBeacon = {
  bench(b) {
    // Each beacon is a POST to the sink service that the request waits for.
    b.run('navigator.sendBeacon(url, string)', () => {
      b.blackBox(navigator.sendBeacon('http://sink/beacon', 'body'));
    });
    b.run('navigator.sendBeacon(url)', () => {
      b.blackBox(navigator.sendBeacon('http://sink/beacon'));
    });
    b.run(`navigator.sendBeacon() throws x${COUNT}`, () => {
      let caught;
      for (let i = 0; i < COUNT; i++) {
        try {
          navigator.sendBeacon();
        } catch (error) {
          caught = error;
        }
      }
      b.blackBox(caught);
    });
    b.run(`sendBeacon.call({}) throws x${COUNT}`, () => {
      const { sendBeacon: method } = navigator;
      const receiver = {};
      let caught;
      for (let i = 0; i < COUNT; i++) {
        try {
          method.call(receiver, 'http://sink/beacon');
        } catch (error) {
          caught = error;
        }
      }
      b.blackBox(caught);
    });
  },
};
