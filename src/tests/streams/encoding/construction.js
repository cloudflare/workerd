// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TextDecoderStream construction: label handling and option defaults.
//
// Divergence: when an options bag is present but omits `fatal`, C++
// defaults it to TRUE unless pedantic_wpt is set (spec says false; the
// encoding-cpp-pedantic cell pins the spec-aligned side, the legacy cell
// pins the quirk). TypeScript always uses the spec default. With no
// options bag at all, everything defaults to false.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl, pedanticWpt } from 'which-impl';

export const decoderOptionsReflection = {
  test() {
    // The label is normalized ('utf-16' names the LE variant) and the
    // options are reflected by the getters.
    const stream = new TextDecoderStream('utf-16', {
      fatal: true,
      ignoreBOM: true,
    });
    strictEqual(stream.encoding, 'utf-16le');
    strictEqual(stream.fatal, true);
    strictEqual(stream.ignoreBOM, true);
  },
};

export const fatalDefaults = {
  test() {
    strictEqual(new TextDecoderStream().fatal, false);
    strictEqual(new TextDecoderStream('utf-8').fatal, false);

    // Options bag without `fatal`: the C++ quirk kicks in unless
    // pedantic_wpt aligns it with the spec.
    const bagDefault = usingTsImpl || pedanticWpt ? false : true;
    strictEqual(new TextDecoderStream('utf-8', {}).fatal, bagDefault);
    strictEqual(
      new TextDecoderStream('utf-8', { ignoreBOM: true }).fatal,
      bagDefault
    );

    // Explicit fatal wins in both.
    strictEqual(new TextDecoderStream('utf-8', { fatal: false }).fatal, false);
    strictEqual(new TextDecoderStream('utf-8', { fatal: true }).fatal, true);

    // ignoreBOM defaults to false everywhere.
    strictEqual(new TextDecoderStream().ignoreBOM, false);
    strictEqual(new TextDecoderStream('utf-8', {}).ignoreBOM, false);
  },
};

export const invalidLabelThrows = {
  test() {
    throws(
      () => new TextDecoderStream('bogus'),
      (err) => {
        strictEqual(err.constructor, RangeError);
        strictEqual(err.message, '"bogus" is not a valid encoding.');
        return true;
      }
    );
  },
};

export const labelNormalization = {
  test() {
    // ASCII whitespace is trimmed and the label lowercased.
    strictEqual(new TextDecoderStream(' UTF-8\t').encoding, 'utf-8');
    strictEqual(new TextDecoderStream('L1').encoding, 'windows-1252');
  },
};
