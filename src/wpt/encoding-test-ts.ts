// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import path from 'node:path';
import { getBindingPath } from 'harness/common';
import { type TestRunnerConfig } from 'harness/harness';

function loadWptResource(relativePath: string): void {
  const bindingPath = getBindingPath(
    path.dirname(globalThis.state.testFileName),
    relativePath
  );
  const code = globalThis.state.env[bindingPath];
  if (typeof code !== 'string') {
    throw new Error(
      `Test file ${bindingPath} not found. Update wpt_test.bzl to handle this case.`
    );
  }
  globalThis.state.env.unsafe.eval(code);
}

export default {
  'api-basics.any.js': {},
  'api-invalid-label.any.js': {},
  'api-replacement-encodings.any.js': {},
  'api-surrogates-utf8.any.js': {},
  'encodeInto.any.js': {
    comment: 'Requires MessageChannel.postMessage transfer list support',
    expectedFailures: ['encodeInto() and a detached output buffer'],
  },
  'idlharness.any.js': {},
  'iso-2022-jp-decoder.any.js': {},
  'legacy-mb-japanese/euc-jp/eucjp-decoder.js': {},
  'legacy-mb-japanese/euc-jp/eucjp-encoder.js': {
    before: (): void => {
      loadWptResource('./jis0208_index.js');
      loadWptResource('./jis0212_index.js');
    },
  },
  'legacy-mb-japanese/euc-jp/jis0208_index.js': {},
  'legacy-mb-japanese/euc-jp/jis0212_index.js': {},
  'legacy-mb-japanese/iso-2022-jp/iso2022jp-decoder.js': {},
  'legacy-mb-japanese/iso-2022-jp/iso2022jp-encoder.js': {
    before: (): void => {
      loadWptResource('./jis0208_index.js');
    },
  },
  'legacy-mb-japanese/iso-2022-jp/jis0208_index.js': {},
  'legacy-mb-japanese/shift_jis/jis0208_index.js': {},
  'legacy-mb-japanese/shift_jis/sjis-decoder.js': {},
  'legacy-mb-japanese/shift_jis/sjis-encoder.js': {
    before: (): void => {
      loadWptResource('./jis0208_index.js');
    },
  },
  'legacy-mb-korean/euc-kr/euckr-decoder.js': {},
  'legacy-mb-korean/euc-kr/euckr-encoder.js': {
    before: (): void => {
      loadWptResource('./euckr_index.js');
    },
  },
  'legacy-mb-korean/euc-kr/euckr_index.js': {},
  'legacy-mb-schinese/gb18030/gb18030-decoder.any.js': {},
  'legacy-mb-schinese/gbk/gbk-decoder.any.js': {},
  'legacy-mb-tchinese/big5/big5-decoder.js': {},
  'legacy-mb-tchinese/big5/big5-encoder.js': {
    before: (): void => {
      loadWptResource('./big5_index.js');
    },
  },
  'legacy-mb-tchinese/big5/big5_index.js': {},
  'replacement-encodings.any.js': {
    comment: 'XMLHttpRequest is not defined',
    disabledTests: [
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
    comment: 'document and XMLHttpRequest are not supported',
    disabledTests: true,
  },
  'streams/backpressure.any.js': {},
  'streams/decode-attributes.any.js': {},
  'streams/decode-bad-chunks.any.js': {},
  'streams/decode-ignore-bom.any.js': {},
  'streams/decode-incomplete-input.any.js': {},
  'streams/decode-non-utf8.any.js': {},
  'streams/decode-split-character.any.js': {},
  'streams/decode-utf8.any.js': {
    comment: 'MessageChannel transfer list is not supported',
    disabledTests: [
      'decoding a transferred Uint8Array chunk should give no output',
      'decoding a transferred ArrayBuffer chunk should give no output',
    ],
  },
  'streams/encode-bad-chunks.any.js': {},
  'streams/encode-utf8.any.js': {},
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
    omittedTests: true,
  },
  'textdecoder-arguments.any.js': {
    comment:
      'TextDecoder does not detach the underlying ArrayBuffer during options argument conversion',
    disabledTests: [
      // Behavior differs between platforms: on Windows this test passes while
      // it fails on Linux. Use disabledTests to avoid the cross-platform mismatch.
      'TextDecoder decode() with array buffer detached during arg conversion',
    ],
  },
  'textdecoder-byte-order-marks.any.js': {},
  'textdecoder-copy.any.js': {},
  'textdecoder-eof.any.js': {},
  'textdecoder-fatal-single-byte.any.js': {},
  'textdecoder-fatal-streaming.any.js': {},
  'textdecoder-fatal.any.js': {},
  'textdecoder-ignorebom.any.js': {},
  'textdecoder-labels.any.js': {},
  'textdecoder-mistakes.any.js': {
    comment: 'iso-2022-jp fatal stream state not preserved after throw',
    disabledTests: [
      // Behavior differs between platforms: on Windows this test passes while
      // it fails on Linux. Use disabledTests to avoid the cross-platform mismatch.
      'fatal stream: iso-2022-jp',
    ],
  },
  'textdecoder-streaming.any.js': {},
  'textdecoder-utf16-surrogates.any.js': {},
  'textencoder-constructor-non-utf.any.js': {},
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
    comment:
      'Browser-only test that requires document/iframe for HTML charset inheritance detection',
    disabledTests: true,
  },
} satisfies TestRunnerConfig;
