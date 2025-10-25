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
    comment: 'See comments on each failure',
    expectedFailures: [
      // Won't implement
      'encodeInto() and a detached output buffer',
    ],
  },
  'idlharness.any.js': {
    comment: 'Test file /resources/WebIDLParser.js not found',
    disabledTests: true,
  },
  'iso-2022-jp-decoder.any.js': {
    comment: 'TODO investigate this',
    // ICU should handle this; see why we fail
    expectedFailures: [
      
      'iso-2022-jp decoder: Error ESC',
      'iso-2022-jp decoder: Katakana ESC, SO / SI',
      'iso-2022-jp decoder: character, error ESC #2',
    ],
  },
  'legacy-mb-japanese/euc-jp/eucjp-decoder.js': {},
  'legacy-mb-japanese/euc-jp/eucjp-encoder.js': {
    // Something wrong with the way files are included
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
    // Something wrong with the way files are included
    comment: 'ReferenceError: euckr is not defined',
    omittedTests: true,
  },
  'legacy-mb-korean/euc-kr/euckr_index.js': {},
  'legacy-mb-schinese/gb18030/gb18030-decoder.any.js': {
    // Figure out what is going on with ICU
    // We're using V8's ICU (are they possibly excluding whacky encodings?)
    comment: 'Too many failures to list individually',
    omittedTests: true,
  },
  'legacy-mb-schinese/gbk/gbk-decoder.any.js': {
    // Figure out what is going on with ICU
    comment: 'Too many failures to list individually',
    omittedTests: true,
  },
  'legacy-mb-tchinese/big5/big5-decoder.js': {},
  'legacy-mb-tchinese/big5/big5-encoder.js': {
    // Bad inclues
    comment: 'big5 is not defined',
    omittedTests: true,
  },
  'legacy-mb-tchinese/big5/big5_index.js': {},
  'replacement-encodings.any.js': {
    // Maybe ask them to use fetch
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
    // Omit this
    comment: 'Too many failures to list individually',
    disabledTests: true,
  },
  'streams/backpressure.any.js': {
    comment: 'Implement step_timeout in harness',
    // We ought to pass them; but first I do need to implement step_timeout
    expectedFailures: [
      'write() should not complete until read relieves backpressure for TextDecoderStream',
      'additional writes should wait for backpressure to be relieved for class TextDecoderStream',
      'write() should not complete until read relieves backpressure for TextEncoderStream',
      'additional writes should wait for backpressure to be relieved for class TextEncoderStream',
    ],
  },
  'streams/decode-attributes.any.js': {
    comment: 'TODO investigate this',
    // Bad argument validation
    expectedFailures: [
      "setting fatal to 'undefined' should set the attribute to false",
      'a throwing fatal member should cause the constructor to throw',
      'a throwing ignoreBOM member should cause the constructor to throw',
    ],
  },
  'streams/decode-bad-chunks.any.js': {
    comment: 'Failed V8 assert',
    // Ticket for this. Our type checking is wrong.
    //  external/v8/src/api/api-inl.h:163; message = v8::internal::ValueHelper::IsEmpty(that) || IsJSArrayBufferView(v8::internal::Tagged<v8::internal::Object>( v8::internal::ValueHelper::ValueAsAddress(that)))
    disabledTests: true,
  },
  'streams/decode-ignore-bom.any.js': {},
  'streams/decode-incomplete-input.any.js': {
    // Create a ticket to investigate
    comment: 'TODO investigate this',
    expectedFailures: [
      'incomplete input with error mode "fatal" should error the stream',
      'incomplete input with error mode "replacement" should end with a replacement character',
    ],
  },
  'streams/decode-non-utf8.any.js': {
    comment: 'TODO investigate this',
    // Do we support fatal true
    // Are we doing something wrong with endianness or malformed UTF-16
    expectedFailures: [
      'TextDecoderStream should be able to decode invalid sequences in UTF-16BE',
      'TextDecoderStream should be able to decode invalid sequences in UTF-16LE',
      'TextDecoderStream should be able to reject invalid sequences in UTF-16BE',
      'TextDecoderStream should be able to reject invalid sequences in UTF-16LE',
    ],
  },
  'streams/decode-split-character.any.js': {
    comment: 'TODO investigate this',
    // Code point split investigatyion
    expectedFailures: [
      'a code point split between chunks should not be emitted until all bytes are available; split point = 2',
      'a code point split between chunks should not be emitted until all bytes are available; split point = 3',
      'a code point split between chunks should not be emitted until all bytes are available; split point = 4',
      'a code point split between chunks should not be emitted until all bytes are available; split point = 5',
      'a code point should be emitted as soon as all bytes are available',
      'an empty chunk inside a code point split between chunks should not change the output; split point = 1',
      'an empty chunk inside a code point split between chunks should not change the output; split point = 2',
      'an empty chunk inside a code point split between chunks should not change the output; split point = 3',
      'an empty chunk inside a code point split between chunks should not change the output; split point = 4',
      'an empty chunk inside a code point split between chunks should not change the output; split point = 5',
      'an empty chunk inside a code point split between chunks should not change the output; split point = 6',
    ],
  },
  'streams/decode-utf8.any.js': {
    comment: 'See comments on each failure',
    expectedFailures: [
      // Enable once MessageChannel is implemented. Mark these as omitted
      // Maybe ask upstream to do this the standard way
      'decoding a transferred Uint8Array chunk should give no output',
      'decoding a transferred ArrayBuffer chunk should give no output',
      // TODO investigate these
      'decoding one UTF-8 chunk should give one output string - ArrayBuffer',
      'decoding an empty chunk should give no output chunks - ArrayBuffer',
      'UTF-8 EOF handling - ArrayBuffer',
      'decoding one UTF-8 chunk should give one output string - SharedArrayBuffer',
      'decoding an empty chunk should give no output chunks - SharedArrayBuffer',
      'UTF-8 EOF handling - SharedArrayBuffer',
      'an initial empty chunk should be ignored - ArrayBuffer',
      'a trailing empty chunk should be ignored - ArrayBuffer',
      'an initial empty chunk should be ignored - SharedArrayBuffer',
      'a trailing empty chunk should be ignored - SharedArrayBuffer',
    ],
  },
  'streams/encode-bad-chunks.any.js': {
    comment: 'TODO investigate this',
    // Likely bad validation
    expectedFailures: [
      'input of type undefined should be converted correctly to string',
      'input of type null should be converted correctly to string',
      'input of type numeric should be converted correctly to string',
      'input of type object should be converted correctly to string',
      'input of type array should be converted correctly to string',
    ],
  },
  'streams/encode-utf8.any.js': {
    comment: 'TODO investigate this',
    // Create tickets
    expectedFailures: [
      'an empty string should result in no output chunk',
      'a character split between chunks should be correctly encoded',
      'a character following one split between chunks should be correctly encoded',
      'an unmatched surrogate at the end of a chunk followed by an astral character in the next chunk should be replaced with the replacement character at the start of the next output chunk',
      'an unmatched surrogate at the end of a chunk followed by an ascii character in the next chunk should be replaced with the replacement character at the start of the next output chunk',
      'a non-terminal unpaired leading surrogate should immediately be replaced',
      "a leading surrogate chunk should error when it is clear it didn't form a pair",
      'a leading empty chunk should be ignored',
      'a trailing empty chunk should be ignored',
      'two consecutive astral characters each split down the middle should be correctly reassembled',
      'two consecutive astral characters each split down the middle with an invalid surrogate in the middle should be correctly encoded',
      'an unmatched surrogate at the end of a chunk followed by a plane 1 character split into two chunks should result in the encoded plane 1 character appearing in the last output chunk',
      'a leading surrogate chunk should be carried past empty chunks',
    ],
  },
  'streams/invalid-realm.window.js': {
    // Disable
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
  'textdecoder-copy.any.js': {
    // Bad type validation; should allow SAB
    comment:
      "Failed to execute 'decode' on 'TextDecoder': parameter 1 is not of type 'Array'.",
    expectedFailures: ['Modify buffer after passing it in (SharedArrayBuffer)'],
  },
  'textdecoder-eof.any.js': {},
  'textdecoder-fatal-single-byte.any.js': {},
  'textdecoder-fatal-streaming.any.js': {},
  'textdecoder-fatal.any.js': {},
  'textdecoder-ignorebom.any.js': {},
  'textdecoder-labels.any.js': {
    // We maybe need to update list of encoding aliases
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
  'textdecoder-utf16-surrogates.any.js': {
    comment: 'Investigate why we are not blocking invalid surrogates',
    // Investigate surrogate problems
    expectedFailures: [
      'utf-16le - lone surrogate lead',
      'utf-16le - lone surrogate lead (fatal flag set)',
      'utf-16le - lone surrogate trail',
      'utf-16le - lone surrogate trail (fatal flag set)',
      'utf-16le - unmatched surrogate lead',
      'utf-16le - unmatched surrogate lead (fatal flag set)',
      'utf-16le - unmatched surrogate trail',
      'utf-16le - unmatched surrogate trail (fatal flag set)',
      'utf-16le - swapped surrogate pair',
      'utf-16le - swapped surrogate pair (fatal flag set)',
    ],
  },
  'textencoder-constructor-non-utf.any.js': {
    comment: 'Investigate this',
    // Probably our alias list
    expectedFailures: [
      'Encoding argument supported for decode: IBM866',
      'Encoding argument supported for decode: Shift_JIS',
      'Encoding argument supported for decode: x-user-defined',
    ],
  },
  'textencoder-utf16-surrogates.any.js': {},
  'unsupported-encodings.any.js': {
    // Ask if they'll take a fetch version
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
    // omit, but see if same coverage as the normal alias tests
    comment: 'Too many failures to list by name',
    disabledTests: true,
  },
} satisfies TestRunnerConfig;
