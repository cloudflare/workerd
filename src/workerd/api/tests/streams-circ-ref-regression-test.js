import fs from 'node:fs';
import { Duplex } from 'node:stream';

// If the test doesn't crash, it passes. There was a bug carried over from
// the old original readable-streams module that didn't translate well from
// the original CommonJS code to the ESM code, leading to a difference in
// behavior when dealing with a circular reference depending on the order
// in which the individual modules were imported.

export const circTest = {
  test() {
    const duplex = new Duplex();
    duplex.destroy();
  },
};
