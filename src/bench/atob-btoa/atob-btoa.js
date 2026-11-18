// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// atob() and btoa(): short, medium and long inputs, the whitespace and
// padding atob() forgives, and the errors both throw. A short call does
// COUNT operations, since one costs about as much as the runner's overhead
// per call.

const COUNT = 16;

// Binary strings of every byte value, and their encodings.
function binary(length) {
  let result = '';
  for (let i = 0; i < length; i++) {
    result += String.fromCharCode((i * 31 + 7) & 0xff);
  }
  return result;
}

const kShort = binary(16);
const kMedium = binary(1024);
const kLong = binary(1024 * 1024);
const kAscii = 'Hello, World! 12';

const kShortEncoded = btoa(kShort);
const kMediumEncoded = btoa(kMedium);
const kLongEncoded = btoa(kLong);
// As MIME wraps it: a CRLF after every 76 characters.
const kMediumWrapped = kMediumEncoded.replace(/.{76}/g, '$&\r\n');
const kUnpadded = kShortEncoded.replace(/=+$/, '');

// A two-byte string whose characters are all Latin-1, as concatenation
// with a character above U+00FF and slicing it off leaves one.
const kTwoByte = (kMedium + 'Ā').slice(0, -1);

export const encode = {
  bench(b) {
    b.run(`btoa(16 bytes) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = btoa(kShort);
      b.blackBox(result);
    });
    b.run(`btoa(16 ASCII characters) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = btoa(kAscii);
      b.blackBox(result);
    });
    b.run('btoa(1 KiB)', () => b.blackBox(btoa(kMedium)));
    b.run('btoa(1 KiB, two-byte string)', () => b.blackBox(btoa(kTwoByte)));
    b.run('btoa(1 MiB)', () => b.blackBox(btoa(kLong)));
    b.run(`btoa(number) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = btoa(1234567890);
      b.blackBox(result);
    });
  },
};

export const decode = {
  bench(b) {
    b.run(`atob(16 bytes) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = atob(kShortEncoded);
      b.blackBox(result);
    });
    b.run(`atob(16 bytes, unpadded) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = atob(kUnpadded);
      b.blackBox(result);
    });
    b.run('atob(1 KiB)', () => b.blackBox(atob(kMediumEncoded)));
    b.run('atob(1 KiB, wrapped lines)', () => b.blackBox(atob(kMediumWrapped)));
    b.run('atob(1 MiB)', () => b.blackBox(atob(kLongEncoded)));
  },
};

export const roundTrip = {
  bench(b) {
    b.run(`atob(btoa(16 bytes)) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = atob(btoa(kShort));
      b.blackBox(result);
    });
    b.run('atob(btoa(1 KiB))', () => b.blackBox(atob(btoa(kMedium))));
  },
};

export const errors = {
  bench(b) {
    b.run(`atob(invalid) x${COUNT}`, () => {
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
    b.run(`btoa(not Latin-1) x${COUNT}`, () => {
      let caught;
      for (let i = 0; i < COUNT; i++) {
        try {
          btoa('Ā');
        } catch (error) {
          caught = error;
        }
      }
      b.blackBox(caught);
    });
  },
};
