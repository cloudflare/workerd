// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Event stream interpretation, through EventSource.from(). Cases named
// after a WPT file are adapted from it (see AGENTS.md). Where the C++
// parser departs from the HTML standard, the TypeScript one follows the
// standard; each such case is a ledger divergence (#4 to #8 in AGENTS.md).

import { deepStrictEqual, strictEqual } from 'node:assert';
import { parse } from 'helpers';
import { usingTsImpl } from 'which-impl';

async function data(chunks) {
  return (await parse(chunks)).messages.map(([, value]) => value);
}

export const dataLinesJoin = {
  async test() {
    deepStrictEqual(await data(['data: one\ndata: two\n\n']), ['one\ntwo']);
  },
};

// Comments, unknown fields and differently-cased field names are ignored.
export const ignoredLines = {
  async test() {
    deepStrictEqual(
      await data([
        ': comment\n' +
          'falsefield:msg\n\n' +
          'Data: data\n\n' +
          'foo: bar\ndata: kept\n\n',
      ]),
      ['kept']
    );
  },
};

// Only one space after the colon is dropped.
export const leadingSpace = {
  async test() {
    deepStrictEqual(await data(['data:  two\n\ndata:x\n\n']), [' two', 'x']);
  },
};

// WPT format-leading-space.any.js.
export const formatLeadingSpace = {
  async test() {
    deepStrictEqual(await data(['data:\ttest\rdata: \ndata:test\n\n']), [
      '\ttest\n\ntest',
    ]);
  },
};

export const lineEndings = {
  async test() {
    deepStrictEqual(await data(['data: a\r\n\r\ndata: b\r\rdata: c\n\n']), [
      'a',
      'b',
      'c',
    ]);
  },
};

// WPT format-newlines.any.js.
export const formatNewlines = {
  async test() {
    deepStrictEqual(await data(['data:test\r\ndata\ndata:test\r\n\r']), [
      'test\n\ntest',
    ]);
  },
};

// WPT format-field-parsing.any.js, with the blank line its server adds.
export const formatFieldParsing = {
  async test() {
    deepStrictEqual(
      await data([
        'data:\0\ndata:  2\rData:1\ndata\0:2\ndata:1\r\0data:4\nda-ta:3\r' +
          'data_5\ndata:3\rdata:\r\n data:32\ndata:4\n\n',
      ]),
      ['\0\n 2\n1\n3\n\n4']
    );
  },
};

// WPT format-null-character.any.js.
export const formatNullCharacter = {
  async test() {
    deepStrictEqual(await data(['data:\0\n\n']), ['\0']);
  },
};

// An event the stream ends before completing is dropped.
export const unterminatedEventIsDropped = {
  async test() {
    deepStrictEqual(await data(['data: a\n\ndata: b']), ['a']);
  },
};

export const leadingBomIsStripped = {
  async test() {
    deepStrictEqual(await data(['\uFEFFdata: first\n\n', 'data: second\n\n']), [
      'first',
      'second',
    ]);
  },
};

// WPT format-bom-2.any.js: only the first BOM is stripped, and a second
// makes the line's field name unknown.
export const formatBom2 = {
  async test() {
    deepStrictEqual(
      await data(['\uFEFF\uFEFFdata:1\n\ndata:2\n\ndata:3\n\n']),
      ['2', '3']
    );
  },
};

export const invalidUtf8IsReplaced = {
  async test() {
    deepStrictEqual(
      await data([
        new Uint8Array([0x64, 0x61, 0x74, 0x61, 0x3a, 0xff, 0x0a, 0x0a]),
      ]),
      ['\uFFFD']
    );
  },
};

export const utf8SplitAcrossChunks = {
  async test() {
    deepStrictEqual(
      await data([
        new Uint8Array([0x64, 0x61, 0x74, 0x61, 0x3a, 0xc3]),
        new Uint8Array([0xa9, 0x0a, 0x0a]),
      ]),
      ['\u00e9']
    );
  },
};

// An `event` field names the next message's type, and only that one's.
export const namedEvents = {
  async test() {
    const { messages } = await parse(
      ['event: foo\ndata: a\n\ndata: b\n\nevent: test\ndata: c\n\n'],
      ['foo', 'test']
    );
    deepStrictEqual(messages, [
      ['foo', 'a', ''],
      ['message', 'b', ''],
      ['test', 'c', ''],
    ]);
  },
};

// The last event ID persists across messages until an `id` field resets it,
// whether that field is empty or has no value at all.
export const lastEventIdPersistsAndResets = {
  async test() {
    const { messages } = await parse([
      'data: one\nid: 1\n\n' +
        'data: two\n\n' +
        'id: 2\ndata: three\n\n' +
        'id: \ndata: four\n\n' +
        'id: 3\ndata: five\n\n' +
        'id\ndata: six\n\n',
    ]);
    deepStrictEqual(
      messages.map(([, value, lastEventId]) => [value, lastEventId]),
      [
        ['one', '1'],
        ['two', '1'],
        ['three', '2'],
        ['four', ''],
        ['five', '3'],
        ['six', ''],
      ]
    );
  },
};

// #4. WPT format-field-event-empty.any.js: an empty `event` field
// dispatches a 'message'. C++: an event of type ''.
export const emptyEventField = {
  async test() {
    const { messages } = await parse(['event: \ndata:data\n\n'], ['']);
    deepStrictEqual(messages, [[usingTsImpl ? 'message' : '', 'data', '']]);
  },
};

// #5. A BOM split across chunks is stripped. C++ strips only a BOM within
// the first chunk, so the first line's field name is unknown.
export const bomSplitAcrossChunks = {
  async test() {
    deepStrictEqual(
      await data([
        new Uint8Array([0xef, 0xbb]),
        new Uint8Array([0xbf]),
        'data: a\n\n',
      ]),
      usingTsImpl ? ['a'] : []
    );
  },
};

// #6. An `id` field containing U+0000 is ignored. C++ takes it.
export const idWithNullIsIgnored = {
  async test() {
    const { messages } = await parse(['id: a\0b\ndata: x\n\n']);
    deepStrictEqual(messages, [['message', 'x', usingTsImpl ? '' : 'a\0b']]);
  },
};

// #7. WPT format-field-data.any.js: a `data` field with an empty value
// dispatches a message with empty data. C++ dispatches nothing for it.
export const formatFieldData = {
  async test() {
    deepStrictEqual(
      await data(['data:\n\ndata\ndata\n\ndata:test\n\n']),
      usingTsImpl ? ['', '\n', 'test'] : ['\n', 'test']
    );
  },
};

// #8. A CRLF split across chunks ends one line. C++ ends two, so the empty
// second one dispatches early.
export const crlfSplitAcrossChunks = {
  async test() {
    deepStrictEqual(
      await data(['data: a\r', '\ndata: b\n\n']),
      usingTsImpl ? ['a\nb'] : ['a', 'b']
    );
  },
};

// Every message read from a stream is dispatched, even when the stream ends
// right after them.
export const errorFollowsTheLastMessage = {
  async test() {
    const { messages, error } = await parse(['data: a\n\ndata: b\n\n']);
    strictEqual(messages.length, 2);
    strictEqual(error.error.message, 'The server disconnected.');
  },
};
