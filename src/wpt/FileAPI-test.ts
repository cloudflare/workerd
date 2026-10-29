// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

const ENDINGS_EXPECTED_FAILURES = [
  'Valid "endings" value: "transparent"',
  'Valid "endings" value: "native"',
  'Invalid "endings" value: null',
  'Invalid "endings" value: ""',
  'Invalid "endings" value: "invalidEnumValue"',
  'Invalid "endings" value: "Transparent"',
  'Invalid "endings" value: "NATIVE"',
  'Invalid "endings" value: 0',
  'Invalid "endings" value: {}',
];

export default {
  'FileReaderSync.worker.js': {
    comment: 'FileReader and FileReaderSync are not implemented',
    omittedTests: true,
  },
  'blob/Blob-array-buffer.any.js': {},
  'blob/Blob-bytes.any.js': {},
  'blob/Blob-constructor-detached-buffer.any.js': {
    comment:
      'The tests detach buffers with a structured-clone transfer list, which workerd rejects with "Transfer list is not supported"',
    expectedFailures: [
      'Blob from a detached ArrayBufferView should be empty',
      'Blob from a detached ArrayBufferView with offset should be empty',
      'Blob from a detached ArrayBuffer should be empty',
      'Blob from a detached ArrayBufferView mixed with string, detached part ignored',
    ],
  },
  'blob/Blob-constructor-dom.window.js': {
    comment: 'Uses DOM platform objects (document, window, HTMLCollection)',
    omittedTests: true,
  },
  'blob/Blob-constructor-endings.any.js': {
    comment:
      'The endings option is unimplemented (jsg::Unimplemented in blob.h), so every value throws Error, even valid ones, instead of TypeError for invalid ones',
    expectedFailures: ENDINGS_EXPECTED_FAILURES,
  },
  'blob/Blob-constructor.any.js': {
    comment:
      'blobParts is converted as kj::Array (Blob::Bits in blob.h), which accepts only real Arrays, ' +
      'instead of as a WebIDL sequence<BlobPart> iterated through @@iterator. So iterables are rejected, ' +
      "a length getter isn't called, and changes made to the array during conversion aren't reflected. " +
      'Transfer lists are not supported by structuredClone. The endings option is unimplemented. ' +
      'normalizeType() in blob.c++ only discards types containing characters below U+0020, so U+007F is kept.',
    expectedFailures: [
      'A plain object with @@iterator should be treated as a sequence for the blobParts argument.',
      'A plain object with custom @@iterator should be treated as a sequence for the blobParts argument.',
      'A plain object with @@iterator and a length property should be treated as a sequence for the blobParts argument.',
      'A String object should be treated as a sequence for the blobParts argument.',
      'A Uint8Array object should be treated as a sequence for the blobParts argument.',
      'The length getter should be invoked and any exceptions should be propagated.',
      'ToUint32 should be applied to the length and any exceptions should be propagated.',
      'Getters and value conversions should happen in order until an exception is thrown.',
      'Changes to the blobParts array should be reflected in the returned Blob (pop).',
      'Changes to the blobParts array should be reflected in the returned Blob (unshift).',
      'Passing a FrozenArray as the blobParts array should work (FrozenArray<MessagePort>).',
      'options properties should be accessed in lexicographic order.',
      'Blob with type "image/gif;\x7F"',
    ],
  },
  'blob/Blob-in-worker.worker.js': {
    comment:
      'importScripts() does not exist; the harness already provides testharness.js, so the call is removed',
    replace: (code: string): string =>
      code.replace('importScripts("/resources/testharness.js");', ''),
  },
  'blob/Blob-newobject.any.js': {},
  'blob/Blob-slice-overflow.any.js': {},
  'blob/Blob-slice.any.js': {
    comment:
      'Blob.slice() takes int start/end (blob.h) and truncates fractional values; the spec uses [Clamp] long long, which rounds half to even. ' +
      'normalizeType() in blob.c++ only discards types containing characters below U+0020, so U+007F is kept.',
    expectedFailures: [
      'Slicing test: slice (1,1).',
      'Slicing test: slice (1,3).',
      'Slicing test: slice (1,5).',
      'Slicing test: slice (1,7).',
      'Slicing test: slice (1,8).',
      'Slicing test: slice (1,9).',
      'Invalid contentType ("te\x7Fxt/plain")',
    ],
  },
  'blob/Blob-stream.any.js': {},
  'blob/Blob-text.any.js': {},
  'blob/Blob-textStream.any.js': {
    comment: 'Blob.prototype.textStream() is not implemented',
    expectedFailures: true,
  },
  'file/File-constructor-endings.any.js': {
    comment:
      'The endings option is unimplemented (jsg::Unimplemented in blob.h), so every value throws Error, even valid ones, instead of TypeError for invalid ones',
    expectedFailures: ENDINGS_EXPECTED_FAILURES,
  },
  'file/File-constructor.any.js': {
    comment:
      'fileBits is converted as kj::Array rather than a WebIDL sequence, so a custom @@iterator is ignored. ' +
      'normalizeType() in blob.c++ only discards types containing characters below U+0020, so U+007F is kept.',
    expectedFailures: [
      'Custom @@iterator',
      'Using type in File constructor: ascii/nonprintable\x7F',
    ],
  },
  'file/Worker-read-file-constructor.worker.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'file/send-file-formdata-controls.any.js': {
    comment: 'Requires wptserve',
    disabledTests: true,
  },
  'file/send-file-formdata-punctuation.any.js': {
    comment: 'Requires wptserve',
    disabledTests: true,
  },
  'file/send-file-formdata-utf-8.any.js': {
    comment: 'Requires wptserve',
    disabledTests: true,
  },
  'file/send-file-formdata.any.js': {
    comment: 'Requires wptserve',
    disabledTests: true,
  },
  'fileReader.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'idlharness.any.js': {
    comment:
      'Blob.text(), arrayBuffer() and bytes() throw synchronously on a bad receiver instead of returning a rejected promise. ' +
      'Blob.textStream() is not implemented. The File interface object does not inherit from Blob and has the wrong length. ' +
      'FileReader is not implemented, and FileList only exists for <input type=file> and drag and drop in browsers.',
    expectedFailures: [
      'Blob interface: operation text()',
      'Blob interface: operation arrayBuffer()',
      'Blob interface: operation bytes()',
      'Blob interface: operation textStream()',
      'Blob interface: new Blob(["TEST"]) must inherit property "textStream()" with the proper type',
      'Blob interface: new File(["myFileBits"], "myFileName") must inherit property "textStream()" with the proper type',
      'File interface: existence and properties of interface object',
      'File interface object length',
    ],
    omittedTests: [/^FileList /, /^FileReader /, /new FileReader\(\)/],
  },
  'idlharness.worker.js': {
    comment:
      'Same IDL checks as idlharness.any.js, plus FileReaderSync, which workerd does not implement',
    omittedTests: true,
  },
  'reading-data-section/Determining-Encoding.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'reading-data-section/FileReader-event-handler-attributes.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'reading-data-section/FileReader-multiple-reads.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'reading-data-section/filereader_abort.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'reading-data-section/filereader_error.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'reading-data-section/filereader_events.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'reading-data-section/filereader_readAsArrayBuffer.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'reading-data-section/filereader_readAsBinaryString.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'reading-data-section/filereader_readAsDataURL.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'reading-data-section/filereader_readAsText.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'reading-data-section/filereader_readAsText_blob_type_charset.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'reading-data-section/filereader_readystate.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'reading-data-section/filereader_result.any.js': {
    comment: 'FileReader is not implemented',
    omittedTests: true,
  },
  'support/Blob.js': {
    comment: 'Helper loaded by other tests via META script',
    omittedTests: true,
  },
  'support/historical-serviceworker.js': {
    comment: 'Helper used by historical.https.html',
    omittedTests: true,
  },
  'support/send-file-form-helper.js': {
    comment: 'Helper used by HTML form submission tests',
    omittedTests: true,
  },
  'support/send-file-formdata-helper.js': {
    comment: 'Helper loaded by the send-file-formdata tests via META script',
    omittedTests: true,
  },
  'unicode.any.js': {
    comment:
      'Strings in blobParts should be converted as USVString, replacing unpaired surrogates with U+FFFD, but Blob::BitsValue uses kj::String (blob.h) and they come back unreplaced',
    expectedFailures: [
      'Test that unpaired surrogates are replaced by Blob constructor',
      'Test that unpaired surrogates are replaced by File constructor',
    ],
  },
  'url/url-charset.window.js': {
    comment: 'Opens blob URLs with window.open()',
    omittedTests: true,
  },
  'url/url-format.any.js': {
    comment:
      'URL.createObjectURL() always throws "not implemented" (url-standard.c++); workerd has no blob URL store',
    expectedFailures: [
      'Generated Blob URLs are unique',
      'Blob URL starts with "blob:"',
      'Blob URL starts with "blob:" for Files',
      'Origin of Blob URL matches our origin',
      'Blob URL parses correctly',
      'Origin of Blob URL matches our origin for Files',
    ],
  },
  'url/url-in-tags-revoke.window.js': {
    comment: 'Loads blob URLs in iframes, windows and <script> tags',
    omittedTests: true,
  },
  'url/url-in-tags.window.js': {
    comment: 'Loads blob URLs in DOM elements',
    omittedTests: true,
  },
  'url/url-reload.window.js': {
    comment: 'Reloads blob URLs in iframes',
    omittedTests: true,
  },
  'url/url-with-fetch.any.js': {
    comment:
      'resources/fetch-tests.js calls URL.createObjectURL() at the top level, which always throws "not implemented" (url-standard.c++), so no subtests get registered',
    disabledTests: true,
  },
  'url/url-with-xhr.any.js': {
    comment: 'Uses XMLHttpRequest, which only exists in browsers',
    omittedTests: true,
  },
} satisfies TestRunnerConfig;
