// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'api-basics.any.js': {},
  'api-invalid-label.any.js': {},
  'api-replacement-encodings.any.js': {},
  'api-surrogates-utf8.any.js': {},
  'encodeInto.any.js': {
    comment: 'Requires MessageChannel.postMessage transfer list support',
    expectedFailures: ['encodeInto() and a detached output buffer'],
  },
  'idlharness.any.js': {
    comment: 'Test file /resources/WebIDLParser.js not found',
    disabledTests: true,
  },
  'iso-2022-jp-decoder.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [
      'iso-2022-jp decoder: Error ESC',
      'iso-2022-jp decoder: Katakana ESC, SO / SI',
      'iso-2022-jp decoder: character, error ESC #2',
    ],
  },
  'legacy-mb-japanese/euc-jp/eucjp-decoder.js': {},
  'legacy-mb-japanese/euc-jp/eucjp-encoder.js': {
    comment: 'ReferenceError: jis0208 is not defined',
    omittedTests: true,
  },
  'legacy-mb-japanese/euc-jp/jis0208_index.js': {},
  'legacy-mb-japanese/euc-jp/jis0212_index.js': {},
  'legacy-mb-japanese/iso-2022-jp/iso2022jp-decoder.js': {},
  'legacy-mb-japanese/iso-2022-jp/iso2022jp-encoder.js': {
    comment:
      'This file is meant to be included by tests and cannot run on its own',
    omittedTests: true,
  },
  'legacy-mb-japanese/iso-2022-jp/jis0208_index.js': {},
  'legacy-mb-japanese/shift_jis/jis0208_index.js': {},
  'legacy-mb-japanese/shift_jis/sjis-decoder.js': {},
  'legacy-mb-japanese/shift_jis/sjis-encoder.js': {
    comment:
      'This file is meant to be included by tests and cannot run on its own',
    omittedTests: true,
  },
  'legacy-mb-korean/euc-kr/euckr-decoder.js': {},
  'legacy-mb-korean/euc-kr/euckr-encoder.js': {
    comment: 'ReferenceError: euckr is not defined',
    omittedTests: true,
  },
  'legacy-mb-korean/euc-kr/euckr_index.js': {},
  'legacy-mb-schinese/gb18030/gb18030-decoder.any.js': {
    comment: 'Too many failures to list individually',
    omittedTests: true,
  },
  'legacy-mb-schinese/gbk/gbk-decoder.any.js': {
    comment: 'Too many failures to list individually',
    omittedTests: true,
  },
  'legacy-mb-tchinese/big5/big5-decoder.js': {},
  'legacy-mb-tchinese/big5/big5-encoder.js': {
    comment: 'big5 is not defined',
    omittedTests: true,
  },
  'legacy-mb-tchinese/big5/big5_index.js': {},
  'replacement-encodings.any.js': {
    comment: 'XMLHttpRequest is not defined',
    expectedFailures: [
      'csiso2022kr - non-empty input decodes to one replacement character.',
      'csiso2022kr - empty input decodes to empty output.',
      'hz-gb-2312 - non-empty input decodes to one replacement character.',
      'hz-gb-2312 - empty input decodes to empty output.',
      'iso-2022-cn - non-empty input decodes to one replacement character.',
      'iso-2022-cn - empty input decodes to empty output.',
      'iso-2022-cn-ext - non-empty input decodes to one replacement character.',
      'iso-2022-cn-ext - empty input decodes to empty output.',
      'iso-2022-kr - non-empty input decodes to one replacement character.',
      'iso-2022-kr - empty input decodes to empty output.',
      'replacement - non-empty input decodes to one replacement character.',
      'replacement - empty input decodes to empty output.',
    ],
  },
  'single-byte-decoder.window.js': {
    comment: 'Too many failures to list individually',
    disabledTests: true,
  },
  'streams/backpressure.any.js': {
    comment: 'Implement step_timeout in harness',
    expectedFailures: [
      'write() should not complete until read relieves backpressure for TextDecoderStream',
      'additional writes should wait for backpressure to be relieved for class TextDecoderStream',
      'write() should not complete until read relieves backpressure for TextEncoderStream',
      'additional writes should wait for backpressure to be relieved for class TextEncoderStream',
    ],
  },
  'streams/decode-attributes.any.js': {},
  'streams/decode-bad-chunks.any.js': {},
  'streams/decode-ignore-bom.any.js': {},
  'streams/decode-incomplete-input.any.js': {},
  'streams/decode-non-utf8.any.js': {},
  'streams/decode-split-character.any.js': {},
  'streams/decode-utf8.any.js': {
    comment: 'Enable once MessageChannel is implemented',
    expectedFailures: [
      'decoding a transferred Uint8Array chunk should give no output',
      'decoding a transferred ArrayBuffer chunk should give no output',
    ],
  },
  'streams/encode-bad-chunks.any.js': {},
  'streams/encode-utf8.any.js': {
    comment: 'Surrogate pair handling across chunks not yet implemented',
    expectedFailures: [
      'a character split between chunks should be correctly encoded',
      'a character following one split between chunks should be correctly encoded',
      'an unmatched surrogate at the end of a chunk followed by an astral character in the next chunk should be replaced with the replacement character at the start of the next output chunk',
      'an unmatched surrogate at the end of a chunk followed by an ascii character in the next chunk should be replaced with the replacement character at the start of the next output chunk',
      'a non-terminal unpaired leading surrogate should immediately be replaced',
      'two consecutive astral characters each split down the middle should be correctly reassembled',
      'two consecutive astral characters each split down the middle with an invalid surrogate in the middle should be correctly encoded',
      'an unmatched surrogate at the end of a chunk followed by a plane 1 character split into two chunks should result in the encoded plane 1 character appearing in the last output chunk',
      'a leading surrogate chunk should be carried past empty chunks',
    ],
  },
  'streams/invalid-realm.window.js': {
    comment: 'Enable when ShadowRealm is supported',
    expectedFailures: [
      'TextDecoderStream: write in detached realm should succeed',
      'TextEncoderStream: write in detached realm should succeed',
      'TextEncoderStream: close in detached realm should succeed',
      'TextDecoderStream: close in detached realm should succeed',
    ],
  },
  'streams/readable-writable-properties.any.js': {},
  'streams/realms.window.js': {
    comment: 'ReferenceError: window is not defined',
    disabledTests: true,
  },
  'textdecoder-arguments.any.js': {},
  'textdecoder-byte-order-marks.any.js': {},
  'textdecoder-copy.any.js': {},
  'textdecoder-eof.any.js': {},
  'textdecoder-fatal-single-byte.any.js': {},
  'textdecoder-fatal-streaming.any.js': {},
  'textdecoder-fatal.any.js': {},
  'textdecoder-ignorebom.any.js': {},
  'textdecoder-labels.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [
      '866 => IBM866',
      'cp866 => IBM866',
      'csibm866 => IBM866',
      'ibm866 => IBM866',
      'csshiftjis => Shift_JIS',
      'ms932 => Shift_JIS',
      'ms_kanji => Shift_JIS',
      'shift-jis => Shift_JIS',
      'shift_jis => Shift_JIS',
      'sjis => Shift_JIS',
      'windows-31j => Shift_JIS',
      'x-sjis => Shift_JIS',
      'x-user-defined => x-user-defined',
    ],
  },
  'textdecoder-streaming.any.js': {},
  'textdecoder-utf16-surrogates.any.js': {},
  'textencoder-constructor-non-utf.any.js': {
    comment: 'Investigate this',
    expectedFailures: [
      'Encoding argument supported for decode: IBM866',
      'Encoding argument supported for decode: Shift_JIS',
      'Encoding argument supported for decode: x-user-defined',
    ],
  },
  'textencoder-utf16-surrogates.any.js': {},
  'unsupported-encodings.any.js': {
    comment: 'XMLHttpRequest is not defined',
    expectedFailures: [
      'UTF-7 should not be supported',
      'utf-7 should not be supported',
      'UTF-32 with BOM should decode as UTF-16LE',
      'UTF-32 with no BOM should decode as UTF-8',
      'utf-32 with BOM should decode as UTF-16LE',
      'utf-32 with no BOM should decode as UTF-8',
      'UTF-32LE with BOM should decode as UTF-16LE',
      'UTF-32LE with no BOM should decode as UTF-8',
      'utf-32le with BOM should decode as UTF-16LE',
      'utf-32le with no BOM should decode as UTF-8',
      'UTF-32be with no BOM should decode as UTF-8',
      'UTF-32be with BOM should decode as UTF-8',
      'utf-32be with no BOM should decode as UTF-8',
      'utf-32be with BOM should decode as UTF-8',
    ],
  },
  'unsupported-labels.window.js': {
    comment: 'Too many failures to list by name',
    disabledTests: true,
  },
} satisfies TestRunnerConfig;
