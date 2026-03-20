import { deepStrictEqual, strictEqual, throws, ok } from 'node:assert';

// Test for the Encoding standard Web API implementation.
// The implementation for these are in api/encoding.{h|c++}

function decodeStreaming(decoder, input) {
  // Test truncation behavior while streaming by feeding the decoder a single byte at a time.
  // Note we don't try-catch here because we don't expect this ever to fail, because we're
  // streaming text.
  let x = '';
  for (let i = 0; i < input.length; ++i) {
    x += decoder.decode(input.slice(i, i + 1), { stream: true });
  }
  x += decoder.decode();
  return x;
}

// From https://developer.mozilla.org/en-US/docs/Web/API/Encoding_API/Encodings
const windows1252Labels = [
  'ansi_x3.4-1968',
  'ascii',
  'cp1252',
  'cp819',
  'csisolatin1',
  'ibm819',
  'iso-8859-1',
  'iso-ir-100',
  'iso8859-1',
  'iso88591',
  'iso_8859-1',
  'iso_8859-1:1987',
  'l1',
  'latin1',
  'us-ascii',
  'windows-1252',
  'x-cp1252',
];

const utf8Labels = ['unicode-1-1-utf-8', 'utf-8', 'utf8'];

export const decodeStreamingTest = {
  test() {
    let results = [];

    for (const label of windows1252Labels) {
      ok(
        new TextDecoder(`${label}`).encoding === 'windows-1252',
        `TextDecoder constructed with '${label}' label to have 'windows-1252' encoding.`
      );
    }
    for (const label of utf8Labels) {
      ok(
        new TextDecoder(`${label}`).encoding === 'utf-8',
        `TextDecoder constructed with '${label}' label to have 'utf-8' encoding.`
      );
    }

    const decoder = new TextDecoder();
    const fatalDecoder = new TextDecoder('utf-8', { fatal: true });
    const fatalIgnoreBomDecoder = new TextDecoder('utf-8', {
      fatal: true,
      ignoreBOM: true,
    });

    ok(decoder.encoding === 'utf-8', "default encoding property to be 'utf-8'");
    ok(decoder.fatal === false, 'default fatal property to be false');
    ok(decoder.ignoreBOM === false, 'default ignoreBOM property to be false');

    const fooCat = new Uint8Array([102, 111, 111, 32, 240, 159, 152, 186]);

    ok(decoder.decode().length === 0, 'decoded undefined array length to be 0');
    ok(
      decoder.decode(fooCat) === 'foo 😺',
      'foo-cat from Uint8Buffer to be foo 😺'
    );
    ok(
      decoder.decode(fooCat.buffer) === 'foo 😺',
      'foo-cat from ArrayBuffer to be foo 😺'
    );

    const twoByteCodePoint = new Uint8Array([0xc2, 0xa2]); // cent sign
    const threeByteCodePoint = new Uint8Array([0xe2, 0x82, 0xac]); // euro sign
    const fourByteCodePoint = new Uint8Array([240, 159, 152, 186]); // cat emoji

    [twoByteCodePoint, threeByteCodePoint, fourByteCodePoint].forEach(
      (input) => {
        // For each input sequence of code units, try decoding each subsequence of code units except the
        // full code point itself.
        for (let i = 1; i < input.length; ++i) {
          const head = input.slice(0, input.length - i);
          const tail = input.slice(input.length - i);

          ok(
            decoder.decode(head) === '�',
            'code point fragment (head) to be replaced with replacement character'
          );
          ok(
            decoder.decode(tail) === '�'.repeat(tail.length),
            'code point fragment (tail) to be replaced with replacement character'
          );

          const errMsg = 'Failed to decode input.';

          // Exception to be thrown decoding code point fragment (tail) in fatal mode
          throws(() => fatalDecoder.decode(head));
          throws(() => fatalDecoder.decode(tail));
        }
      }
    );

    // Test ASCII
    const asciiDecoder = new TextDecoder('ascii');
    ok(
      asciiDecoder.decode().length === 0,
      'decoded undefined array length to be 0'
    );
    ok(
      asciiDecoder.decode(new Uint8Array([162, 174, 255])) === '¢®ÿ',
      'decoded extended ascii correctly'
    );

    // Test streaming

    ok(
      decodeStreaming(fatalDecoder, twoByteCodePoint) === '¢',
      '2-byte code point (cent sign) to be decoded correctly'
    );
    ok(
      decodeStreaming(fatalDecoder, threeByteCodePoint) === '€',
      '3-byte code point (euro sign) to be decoded correctly'
    );
    ok(
      decodeStreaming(fatalDecoder, fourByteCodePoint) === '😺',
      '4-byte code point (cat emoji) to be decoded correctly'
    );

    const bom = new Uint8Array([0xef, 0xbb, 0xbf]);
    const bomBom = new Uint8Array([0xef, 0xbb, 0xbf, 0xef, 0xbb, 0xbf]);

    ok(
      decodeStreaming(fatalDecoder, bom) === '',
      'BOM to be stripped by TextDecoder without ignoreBOM set'
    );
    ok(
      decodeStreaming(fatalDecoder, bomBom) === '\ufeff',
      'first BOM to be stripped by TextDecoder without ignoreBOM set'
    );

    ok(
      decodeStreaming(fatalIgnoreBomDecoder, bom) === '\ufeff',
      'BOM not to be stripped by TextDecoder with ignoreBOM set'
    );
    ok(
      decodeStreaming(fatalIgnoreBomDecoder, bomBom) === '\ufeff\ufeff',
      'first BOM not to be stripped by TextDecoder with ignoreBOM set'
    );

    const shiftJisDecoder = new TextDecoder('shift_jis');
    let shiftJisResult = '';
    shiftJisResult += shiftJisDecoder.decode(new Uint8Array([0x82]), {
      stream: true,
    });
    shiftJisResult += shiftJisDecoder.decode(new Uint8Array(), {
      stream: true,
    });
    shiftJisResult += shiftJisDecoder.decode(new Uint8Array([0xa0]), {
      stream: true,
    });
    shiftJisResult += shiftJisDecoder.decode();
    ok(
      shiftJisResult === 'あ',
      'streaming Shift_JIS should tolerate empty chunks without flushing state'
    );
  },
};

export const textEncoderTest = {
  test() {
    const encoder = new TextEncoder();
    strictEqual(encoder.encoding, 'utf-8');
    strictEqual(encoder.encode().length, 0);
    deepStrictEqual(
      encoder.encode('foo 😺'),
      new Uint8Array([102, 111, 111, 32, 240, 159, 152, 186])
    );
  },
};

export const encodeWptTest = {
  test() {
    w3cTestEncode();
    w3cTestEncodeInto();

    function w3cTestEncode() {
      const bad = [
        {
          input: '\uD800',
          expected: '\uFFFD',
          name: 'lone surrogate lead',
        },
        {
          input: '\uDC00',
          expected: '\uFFFD',
          name: 'lone surrogate trail',
        },
        {
          input: '\uD800\u0000',
          expected: '\uFFFD\u0000',
          name: 'unmatched surrogate lead',
        },
        {
          input: '\uDC00\u0000',
          expected: '\uFFFD\u0000',
          name: 'unmatched surrogate trail',
        },
        {
          input: '\uDC00\uD800',
          expected: '\uFFFD\uFFFD',
          name: 'swapped surrogate pair',
        },
        {
          input: '\uD834\uDD1E',
          expected: '\uD834\uDD1E',
          name: 'properly encoded MUSICAL SYMBOL G CLEF (U+1D11E)',
        },
      ];

      bad.forEach(function (t) {
        const encoded = new TextEncoder().encode(t.input);
        const decoded = new TextDecoder().decode(encoded);
        strictEqual(decoded, t.expected);
      });

      strictEqual(new TextEncoder().encode().length, 0);
    }

    function w3cTestEncodeInto() {
      [
        {
          input: 'Hi',
          read: 0,
          destinationLength: 0,
          written: [],
        },
        {
          input: 'A',
          read: 1,
          destinationLength: 10,
          written: [0x41],
        },
        {
          input: '\u{1D306}', // "\uD834\uDF06"
          read: 2,
          destinationLength: 4,
          written: [0xf0, 0x9d, 0x8c, 0x86],
        },
        {
          input: '\u{1D306}A',
          read: 0,
          destinationLength: 3,
          written: [],
        },
        {
          input: '\uD834A\uDF06A¥Hi',
          read: 5,
          destinationLength: 10,
          written: [0xef, 0xbf, 0xbd, 0x41, 0xef, 0xbf, 0xbd, 0x41, 0xc2, 0xa5],
        },
        {
          input: 'A\uDF06',
          read: 2,
          destinationLength: 4,
          written: [0x41, 0xef, 0xbf, 0xbd],
        },
        {
          input: '¥¥',
          read: 2,
          destinationLength: 4,
          written: [0xc2, 0xa5, 0xc2, 0xa5],
        },
      ].forEach((testData) => {
        [
          {
            bufferIncrease: 0,
            destinationOffset: 0,
            filler: 0,
          },
          {
            bufferIncrease: 10,
            destinationOffset: 4,
            filler: 0,
          },
          {
            bufferIncrease: 0,
            destinationOffset: 0,
            filler: 0x80,
          },
          {
            bufferIncrease: 10,
            destinationOffset: 4,
            filler: 0x80,
          },
          {
            bufferIncrease: 0,
            destinationOffset: 0,
            filler: 'random',
          },
          {
            bufferIncrease: 10,
            destinationOffset: 4,
            filler: 'random',
          },
        ].forEach((destinationData) => {
          const bufferLength =
            testData.destinationLength + destinationData.bufferIncrease;
          const destinationOffset = destinationData.destinationOffset;
          const destinationLength = testData.destinationLength;
          const destinationFiller = destinationData.filler;
          const encoder = new TextEncoder();
          const buffer = new ArrayBuffer(bufferLength);
          const view = new Uint8Array(
            buffer,
            destinationOffset,
            destinationLength
          );
          const fullView = new Uint8Array(buffer);
          const control = Array.from({ length: bufferLength }, () => 0);
          let byte = destinationFiller;
          for (let i = 0; i < bufferLength; i++) {
            if (destinationFiller === 'random') {
              byte = Math.floor(Math.random() * 256);
            }
            control[i] = byte;
            fullView[i] = byte;
          }

          // It's happening
          const result = encoder.encodeInto(testData.input, view);

          // Basics
          strictEqual(view.byteLength, destinationLength);
          strictEqual(view.length, destinationLength);

          // Remainder
          strictEqual(result.read, testData.read);
          strictEqual(result.written, testData.written.length);
          for (let i = 0; i < bufferLength; i++) {
            if (
              i < destinationOffset ||
              i >= destinationOffset + testData.written.length
            ) {
              strictEqual(fullView[i], control[i]);
            } else {
              strictEqual(fullView[i], testData.written[i - destinationOffset]);
            }
          }
        });
      });

      [
        DataView,
        Int8Array,
        Int16Array,
        Int32Array,
        Uint16Array,
        Uint32Array,
        Uint8ClampedArray,
        Float16Array,
        Float32Array,
        Float64Array,
      ].forEach((view) => {
        const enc = new TextEncoder();
        throws(() => enc.encodeInto('', new view(new ArrayBuffer(0))));
      });

      throws(() => enc.encodeInto('', new ArrayBuffer(0)));
    }
  },
};

export const big5 = {
  test() {
    // Input is the Big5 encoding for the word 中國人 (meaning Chinese Person)
    // Check is the UTF-8 encoding for the same word.
    const input = new Uint8Array([0xa4, 0xa4, 0xb0, 0xea, 0xa4, 0x48]);
    const check = new Uint8Array([
      0xe4, 0xb8, 0xad, 0xe5, 0x9c, 0x8b, 0xe4, 0xba, 0xba,
    ]);
    const enc = new TextEncoder();
    const dec = new TextDecoder('big5', { ignoreBOM: true });
    const result = enc.encode(dec.decode(input));
    strictEqual(result.length, check.length);
    for (let n = 0; n < result.length; n++) {
      strictEqual(result[n], check[n]);
    }
  },
};

export const utf16leFastTrack = {
  test() {
    // Input is the UTF-16le encoding for the word Hello. This should trigger the fast-path
    // which should handle the encoding with no problems. Here we only test that the results
    // are expected. We cannot verify here that the fast track is actually used.
    const input = new Uint8Array([
      0x68, 0x00, 0x65, 0x00, 0x6c, 0x00, 0x6c, 0x00, 0x6f, 0x00,
    ]);
    const dec = new TextDecoder('utf-16le');
    strictEqual(dec.decode(input), 'hello');
  },
};

export const allTheDecoders = {
  test() {
    [
      ['unicode-1-1-utf-8', 'utf-8'],
      ['unicode11utf8', 'utf-8'],
      ['unicode20utf8', 'utf-8'],
      ['utf-8', 'utf-8'],
      ['utf8', 'utf-8'],
      ['x-unicode20utf8', 'utf-8'],
      ['866', 'ibm866'],
      ['cp866', 'ibm866'],
      ['csibm866', 'ibm866'],
      ['ibm866', 'ibm866'],
      ['csisolatin2', 'iso-8859-2'],
      ['iso-8859-2', 'iso-8859-2'],
      ['iso-ir-101', 'iso-8859-2'],
      ['iso8859-2', 'iso-8859-2'],
      ['iso88592', 'iso-8859-2'],
      ['iso_8859-2', 'iso-8859-2'],
      ['iso_8859-2:1987', 'iso-8859-2'],
      ['l2', 'iso-8859-2'],
      ['latin2', 'iso-8859-2'],
      ['csisolatin3', 'iso-8859-3'],
      ['iso-8859-3', 'iso-8859-3'],
      ['iso-ir-109', 'iso-8859-3'],
      ['iso8859-3', 'iso-8859-3'],
      ['iso88593', 'iso-8859-3'],
      ['iso_8859-3', 'iso-8859-3'],
      ['iso_8859-3:1988', 'iso-8859-3'],
      ['l3', 'iso-8859-3'],
      ['latin3', 'iso-8859-3'],
      ['csisolatin4', 'iso-8859-4'],
      ['iso-8859-4', 'iso-8859-4'],
      ['iso-ir-110', 'iso-8859-4'],
      ['iso8859-4', 'iso-8859-4'],
      ['iso88594', 'iso-8859-4'],
      ['iso_8859-4', 'iso-8859-4'],
      ['iso_8859-4:1988', 'iso-8859-4'],
      ['l4', 'iso-8859-4'],
      ['latin4', 'iso-8859-4'],
      ['csisolatincyrillic', 'iso-8859-5'],
      ['cyrillic', 'iso-8859-5'],
      ['iso-8859-5', 'iso-8859-5'],
      ['iso-ir-144', 'iso-8859-5'],
      ['iso8859-5', 'iso-8859-5'],
      ['iso88595', 'iso-8859-5'],
      ['iso_8859-5', 'iso-8859-5'],
      ['iso_8859-5:1988', 'iso-8859-5'],
      ['arabic', 'iso-8859-6'],
      ['asmo-708', 'iso-8859-6'],
      ['csiso88596e', 'iso-8859-6'],
      ['csiso88596i', 'iso-8859-6'],
      ['csisolatinarabic', 'iso-8859-6'],
      ['ecma-114', 'iso-8859-6'],
      ['iso-8859-6', 'iso-8859-6'],
      ['iso-8859-6-e', 'iso-8859-6'],
      ['iso-8859-6-i', 'iso-8859-6'],
      ['iso-ir-127', 'iso-8859-6'],
      ['iso8859-6', 'iso-8859-6'],
      ['iso88596', 'iso-8859-6'],
      ['iso_8859-6', 'iso-8859-6'],
      ['iso_8859-6:1987', 'iso-8859-6'],
      ['csisolatingreek', 'iso-8859-7'],
      ['ecma-118', 'iso-8859-7'],
      ['elot_928', 'iso-8859-7'],
      ['greek', 'iso-8859-7'],
      ['greek8', 'iso-8859-7'],
      ['iso-8859-7', 'iso-8859-7'],
      ['iso-ir-126', 'iso-8859-7'],
      ['iso8859-7', 'iso-8859-7'],
      ['iso88597', 'iso-8859-7'],
      ['iso_8859-7', 'iso-8859-7'],
      ['iso_8859-7:1987', 'iso-8859-7'],
      ['sun_eu_greek', 'iso-8859-7'],
      ['csiso88598e', 'iso-8859-8'],
      ['csisolatinhebrew', 'iso-8859-8'],
      ['hebrew', 'iso-8859-8'],
      ['iso-8859-8', 'iso-8859-8'],
      ['iso-8859-8-e', 'iso-8859-8'],
      ['iso-ir-138', 'iso-8859-8'],
      ['iso8859-8', 'iso-8859-8'],
      ['iso88598', 'iso-8859-8'],
      ['iso_8859-8', 'iso-8859-8'],
      ['iso_8859-8:1988', 'iso-8859-8'],
      ['visual', 'iso-8859-8'],
      ['csiso88598i', 'iso-8859-8-i'],
      ['iso-8859-8-i', 'iso-8859-8-i'],
      ['logical', 'iso-8859-8-i'],
      ['csisolatin6', 'iso-8859-10'],
      ['iso-8859-10', 'iso-8859-10'],
      ['iso-ir-157', 'iso-8859-10'],
      ['iso8859-10', 'iso-8859-10'],
      ['iso885910', 'iso-8859-10'],
      ['l6', 'iso-8859-10'],
      ['latin6', 'iso-8859-10'],
      ['iso-8859-13', 'iso-8859-13'],
      ['iso8859-13', 'iso-8859-13'],
      ['iso885913', 'iso-8859-13'],
      ['iso-8859-14', 'iso-8859-14'],
      ['iso8859-14', 'iso-8859-14'],
      ['iso885914', 'iso-8859-14'],
      ['csisolatin9', 'iso-8859-15'],
      ['iso-8859-15', 'iso-8859-15'],
      ['iso8859-15', 'iso-8859-15'],
      ['iso885915', 'iso-8859-15'],
      ['iso_8859-15', 'iso-8859-15'],
      ['l9', 'iso-8859-15'],
      ['iso-8859-16', 'iso-8859-16'],
      ['cskoi8r', 'koi8-r'],
      ['koi', 'koi8-r'],
      ['koi8', 'koi8-r'],
      ['koi8-r', 'koi8-r'],
      ['koi8_r', 'koi8-r'],
      ['koi8-ru', 'koi8-u'],
      ['koi8-u', 'koi8-u'],
      ['csmacintosh', 'macintosh'],
      ['mac', 'macintosh'],
      ['macintosh', 'macintosh'],
      ['x-mac-roman', 'macintosh'],
      ['dos-874', 'windows-874'],
      ['iso-8859-11', 'windows-874'],
      ['iso8859-11', 'windows-874'],
      ['iso885911', 'windows-874'],
      ['tis-620', 'windows-874'],
      ['windows-874', 'windows-874'],
      ['cp1250', 'windows-1250'],
      ['windows-1250', 'windows-1250'],
      ['x-cp1250', 'windows-1250'],
      ['cp1251', 'windows-1251'],
      ['windows-1251', 'windows-1251'],
      ['x-cp1251', 'windows-1251'],
      ['ansi_x3.4-1968', 'windows-1252'],
      ['ascii', 'windows-1252'],
      ['cp1252', 'windows-1252'],
      ['cp819', 'windows-1252'],
      ['csisolatin1', 'windows-1252'],
      ['ibm819', 'windows-1252'],
      ['iso-8859-1', 'windows-1252'],
      ['iso-ir-100', 'windows-1252'],
      ['iso8859-1', 'windows-1252'],
      ['iso88591', 'windows-1252'],
      ['iso_8859-1', 'windows-1252'],
      ['iso_8859-1:1987', 'windows-1252'],
      ['l1', 'windows-1252'],
      ['latin1', 'windows-1252'],
      ['us-ascii', 'windows-1252'],
      ['windows-1252', 'windows-1252'],
      ['x-cp1252', 'windows-1252'],
      ['cp1253', 'windows-1253'],
      ['windows-1253', 'windows-1253'],
      ['x-cp1253', 'windows-1253'],
      ['cp1254', 'windows-1254'],
      ['csisolatin5', 'windows-1254'],
      ['iso-8859-9', 'windows-1254'],
      ['iso-ir-148', 'windows-1254'],
      ['iso8859-9', 'windows-1254'],
      ['iso88599', 'windows-1254'],
      ['iso_8859-9', 'windows-1254'],
      ['iso_8859-9:1989', 'windows-1254'],
      ['l5', 'windows-1254'],
      ['latin5', 'windows-1254'],
      ['windows-1254', 'windows-1254'],
      ['x-cp1254', 'windows-1254'],
      ['cp1255', 'windows-1255'],
      ['windows-1255', 'windows-1255'],
      ['x-cp1255', 'windows-1255'],
      ['cp1256', 'windows-1256'],
      ['windows-1256', 'windows-1256'],
      ['x-cp1256', 'windows-1256'],
      ['cp1257', 'windows-1257'],
      ['windows-1257', 'windows-1257'],
      ['x-cp1257', 'windows-1257'],
      ['cp1258', 'windows-1258'],
      ['windows-1258', 'windows-1258'],
      ['x-cp1258', 'windows-1258'],
      ['x-mac-cyrillic', 'x-mac-cyrillic'],
      ['x-mac-ukrainian', 'x-mac-cyrillic'],
      ['chinese', 'gbk'],
      ['csgb2312', 'gbk'],
      ['csiso58gb231280', 'gbk'],
      ['gb2312', 'gbk'],
      ['gb_2312', 'gbk'],
      ['gb_2312-80', 'gbk'],
      ['gbk', 'gbk'],
      ['iso-ir-58', 'gbk'],
      ['x-gbk', 'gbk'],
      ['gb18030', 'gb18030'],
      ['big5', 'big5'],
      ['big5-hkscs', 'big5'],
      ['cn-big5', 'big5'],
      ['csbig5', 'big5'],
      ['x-x-big5', 'big5'],
      ['cseucpkdfmtjapanese', 'euc-jp'],
      ['euc-jp', 'euc-jp'],
      ['x-euc-jp', 'euc-jp'],
      ['csiso2022jp', 'iso-2022-jp'],
      ['iso-2022-jp', 'iso-2022-jp'],
      ['csshiftjis', 'shift_jis'],
      ['ms932', 'shift_jis'],
      ['ms_kanji', 'shift_jis'],
      ['shift-jis', 'shift_jis'],
      ['shift_jis', 'shift_jis'],
      ['sjis', 'shift_jis'],
      ['windows-31j', 'shift_jis'],
      ['x-sjis', 'shift_jis'],
      ['cseuckr', 'euc-kr'],
      ['csksc56011987', 'euc-kr'],
      ['euc-kr', 'euc-kr'],
      ['iso-ir-149', 'euc-kr'],
      ['korean', 'euc-kr'],
      ['ks_c_5601-1987', 'euc-kr'],
      ['ks_c_5601-1989', 'euc-kr'],
      ['ksc5601', 'euc-kr'],
      ['ksc_5601', 'euc-kr'],
      ['windows-949', 'euc-kr'],
      ['csiso2022kr', undefined],
      ['hz-gb-2312', undefined],
      ['iso-2022-cn', undefined],
      ['iso-2022-cn-ext', undefined],
      ['iso-2022-kr', undefined],
      ['replacement', undefined],
      ['unicodefffe', 'utf-16be'],
      ['utf-16be', 'utf-16be'],
      ['csunicode', 'utf-16le'],
      ['iso-10646-ucs-2', 'utf-16le'],
      ['ucs-2', 'utf-16le'],
      ['unicode', 'utf-16le'],
      ['unicodefeff', 'utf-16le'],
      ['utf-16', 'utf-16le'],
      ['utf-16le', 'utf-16le'],
      ['x-user-defined', 'x-user-defined'],
      // Test that match is case-insensitive
      ['UTF-8', 'utf-8'],
      ['UtF-8', 'utf-8'],
    ].forEach((pair) => {
      const [label, key] = pair;
      if (key === undefined) {
        throws(() => new TextDecoder(label));
      } else {
        {
          const dec = new TextDecoder(label);
          strictEqual(dec.encoding, key);
        }
        {
          // Whitespace leading and trailing the label will be ignored.
          const dec = new TextDecoder(`\t\n\r ${label}\t\n\r`);
          strictEqual(dec.encoding, key);
        }
      }
    });
  },
};

// Test that windows-1252 correctly decodes bytes 0x80-0x9F.
// These bytes differ between windows-1252 and ISO-8859-1/Latin-1.
// Per the WHATWG Encoding Standard, labels like "ascii", "latin1", and
// "iso-8859-1" all map to windows-1252, so they must produce the
// windows-1252 code points — not the raw byte values (C1 control chars).
// See: https://encoding.spec.whatwg.org/#names-and-labels
export const windows1252Decode = {
  test() {
    // The windows-1252 mapping for bytes 0x80-0x9F per the WHATWG Encoding Standard.
    // See: https://encoding.spec.whatwg.org/index-windows-1252.txt
    // Bytes 0x81, 0x8D, 0x8F, 0x90, 0x9D map to their C1 control character
    // code points (identity) rather than being undefined.
    const win1252UpperTable = {
      0x80: 0x20ac, // € Euro Sign
      0x81: 0x0081, // <control>
      0x82: 0x201a, // ‚ Single Low-9 Quotation Mark
      0x83: 0x0192, // ƒ Latin Small Letter F With Hook
      0x84: 0x201e, // „ Double Low-9 Quotation Mark
      0x85: 0x2026, // … Horizontal Ellipsis
      0x86: 0x2020, // † Dagger
      0x87: 0x2021, // ‡ Double Dagger
      0x88: 0x02c6, // ˆ Modifier Letter Circumflex Accent
      0x89: 0x2030, // ‰ Per Mille Sign
      0x8a: 0x0160, // Š Latin Capital Letter S With Caron
      0x8b: 0x2039, // ‹ Single Left-Pointing Angle Quotation Mark
      0x8c: 0x0152, // Œ Latin Capital Ligature OE
      0x8d: 0x008d, // <control>
      0x8e: 0x017d, // Ž Latin Capital Letter Z With Caron
      0x8f: 0x008f, // <control>
      0x90: 0x0090, // <control>
      0x91: 0x2018, // ' Left Single Quotation Mark
      0x92: 0x2019, // ' Right Single Quotation Mark
      0x93: 0x201c, // " Left Double Quotation Mark
      0x94: 0x201d, // " Right Double Quotation Mark
      0x95: 0x2022, // • Bullet
      0x96: 0x2013, // – En Dash
      0x97: 0x2014, // — Em Dash
      0x98: 0x02dc, // ˜ Small Tilde
      0x99: 0x2122, // ™ Trade Mark Sign
      0x9a: 0x0161, // š Latin Small Letter S With Caron
      0x9b: 0x203a, // › Single Right-Pointing Angle Quotation Mark
      0x9c: 0x0153, // œ Latin Small Ligature OE
      0x9d: 0x009d, // <control>
      0x9e: 0x017e, // ž Latin Small Letter Z With Caron
      0x9f: 0x0178, // Ÿ Latin Capital Letter Y With Diaeresis
    };

    // Test with all aliases that map to windows-1252
    for (const label of [
      'windows-1252',
      'ascii',
      'latin1',
      'iso-8859-1',
      'us-ascii',
    ]) {
      const decoder = new TextDecoder(label);
      for (const [byte, expectedCodePoint] of Object.entries(
        win1252UpperTable
      )) {
        const byteVal = Number(byte);
        const decoded = decoder.decode(Uint8Array.of(byteVal));
        const actualCodePoint = decoded.codePointAt(0);
        strictEqual(
          actualCodePoint,
          expectedCodePoint,
          `${label}: byte 0x${byteVal.toString(16)} should decode to ` +
            `U+${expectedCodePoint.toString(16).toUpperCase().padStart(4, '0')}, ` +
            `got U+${actualCodePoint.toString(16).toUpperCase().padStart(4, '0')}`
        );
      }
    }

    // Verify that bytes outside 0x80-0x9F still work correctly
    const decoder = new TextDecoder('windows-1252');
    // ASCII range (0x00-0x7F) should be identity
    strictEqual(decoder.decode(Uint8Array.of(0x41)).codePointAt(0), 0x41); // 'A'
    strictEqual(decoder.decode(Uint8Array.of(0x7f)).codePointAt(0), 0x7f);
    // 0xA0-0xFF range is identical between latin-1 and windows-1252
    strictEqual(decoder.decode(Uint8Array.of(0xa2)).codePointAt(0), 0xa2); // ¢
    strictEqual(decoder.decode(Uint8Array.of(0xff)).codePointAt(0), 0xff); // ÿ
  },
};

// Per the WHATWG encoding spec (section 10.1.1), GBK's decoder is gb18030's decoder.
// The .encoding property must still return "gbk", but decoding results must match gb18030.
// https://encoding.spec.whatwg.org/#gbk-decoder
export const gbkDecoderIsGb18030Decoder = {
  test() {
    const gbk = new TextDecoder('gbk');
    const gb18030 = new TextDecoder('gb18030');

    // .encoding property must still distinguish the two
    strictEqual(gbk.encoding, 'gbk');
    strictEqual(gb18030.encoding, 'gb18030');

    // Decoding results must be identical. These byte pairs exercise boundary
    // conditions where gbk and gb18030 would diverge if the ICU converter
    // for gbk were used directly instead of delegating to gb18030.
    const testBytes = [
      [0, 255],
      [128, 255],
      [129, 48],
      [129, 255],
      [254, 48],
      [254, 255],
      [255, 0],
      [255, 255],
    ];
    for (const bytes of testBytes) {
      const u8 = Uint8Array.from(bytes);
      strictEqual(
        gbk.decode(u8),
        gb18030.decode(u8),
        `gbk and gb18030 must decode [${bytes}] identically`
      );
    }
  },
};

const gbVersionAndRangesTest = (encoding) => {
  const loose = new TextDecoder(encoding);
  const checkAll = (...list) => list.forEach((x) => check(...x));
  const check = (bytes, str, invalid = false) => {
    const fatal = new TextDecoder(encoding, { fatal: true });
    const u8 = Uint8Array.from(bytes);
    strictEqual(loose.decode(u8), str);
    if (!invalid) strictEqual(fatal.decode(u8), str);
    if (invalid) throws(() => fatal.decode(u8));
  };

  check([0x84, 0x31, 0xa4, 0x36], '\uFFFC');
  check([0x84, 0x31, 0xa4, 0x37], '\uFFFD');
  check([0x84, 0x31, 0xa4, 0x38], '\uFFFE');
  check([0x84, 0x31, 0xa4, 0x39], '\uFFFF');
  check([0x84, 0x31, 0xa5, 0x30], '\uFFFD', true);
  check([0x8f, 0x39, 0xfe, 0x39], '\uFFFD', true);
  check([0x90, 0x30, 0x81, 0x30], String.fromCodePoint(0x1_00_00));
  check([0x90, 0x30, 0x81, 0x31], String.fromCodePoint(0x1_00_01));

  check([0xe3, 0x32, 0x9a, 0x35], String.fromCodePoint(0x10_ff_ff));
  check([0xe3, 0x32, 0x9a, 0x36], '\uFFFD', true);
  check([0xe3, 0x32, 0x9a, 0x37], '\uFFFD', true);

  check([0xfe, 0x39, 0xfe, 0x39], '\uFFFD', true);
  check([0xff, 0x39, 0xfe, 0x39], '\uFFFD9\uFFFD', true);
  check([0xfe, 0x40, 0xfe, 0x39], '\uFA0C\uFFFD', true);
  check([0xfe, 0x39, 0xff, 0x39], '\uFFFD9\uFFFD9', true);
  check([0xfe, 0x39, 0xfe, 0x40], '\uFFFD9\uFA0C', true);

  checkAll(
    [[0xa8, 0xbb], '\u0251'],
    [[0xa8, 0xbc], '\u1E3F'],
    [[0xa8, 0xbd], '\u0144']
  );
  check([0x81, 0x35, 0xf4, 0x36], '\u1E3E');
  check([0x81, 0x35, 0xf4, 0x37], '\uE7C7');
  check([0x81, 0x35, 0xf4, 0x38], '\u1E40');

  checkAll(
    [[0xa6, 0xd9], '\uFE10'],
    [[0xa6, 0xed], '\uFE18'],
    [[0xa6, 0xf3], '\uFE19']
  );
  checkAll([[0xfe, 0x59], '\u9FB4'], [[0xfe, 0xa0], '\u9FBB']);
};

export const gb18030VersionAndRanges = {
  test() {
    gbVersionAndRangesTest('gb18030');
  },
};

export const gbkVersionAndRanges = {
  test() {
    gbVersionAndRangesTest('gbk');
  },
};

// Verify that the WHATWG-required mapping corrections also produce the
// correct output when the corrected byte sequences appear inside a larger
// buffer (surrounded by ASCII), not only when they are the entire input.
export const gb18030OverridesEmbedded = {
  test() {
    const d = new TextDecoder('gb18030');

    // 0x80 → U+20AC (Euro sign) surrounded by ASCII
    strictEqual(d.decode(Uint8Array.of(0x41, 0x80, 0x42)), 'A\u20ACB');

    // Two-byte mapping corrections surrounded by ASCII
    strictEqual(d.decode(Uint8Array.of(0x41, 0xa8, 0xbb, 0x42)), 'A\u0251B');
    strictEqual(d.decode(Uint8Array.of(0x41, 0xa8, 0xbc, 0x42)), 'A\u1E3FB');
    strictEqual(d.decode(Uint8Array.of(0x41, 0xa8, 0xbd, 0x42)), 'A\u0144B');

    // Vertical form corrections surrounded by ASCII
    strictEqual(d.decode(Uint8Array.of(0x41, 0xa6, 0xd9, 0x42)), 'A\uFE10B');

    // CJK extension corrections surrounded by ASCII
    strictEqual(d.decode(Uint8Array.of(0x41, 0xfe, 0x59, 0x42)), 'A\u9FB4B');
  },
};

export const replacementPushbackAsciiCharactersLoose = {
  test() {
    const vectors = {
      big5: [
        [[0x80], '\uFFFD'],
        [[0x81, 0x40], '\uFFFD@'],
        [[0x83, 0x5c], '\uFFFD\\'],
        [[0x87, 0x87, 0x40], '\uFFFD@'],
        [[0x81, 0x81], '\uFFFD'],
      ],
      'iso-2022-jp': [
        [[0x1b, 0x24], '\uFFFD$'],
        [[0x1b, 0x24, 0x40, 0x1b, 0x24], '\uFFFD\uFFFD'],
      ],
      'euc-jp': [
        [[0x80], '\uFFFD'],
        [[0x8d, 0x8d], '\uFFFD\uFFFD'],
        [[0x8e, 0x8e], '\uFFFD'],
      ],
    };

    for (const [encoding, list] of Object.entries(vectors)) {
      const d = new TextDecoder(encoding);
      for (const [bytes, text] of list) {
        strictEqual(d.decode(Uint8Array.from(bytes)), text);
      }
    }
  },
};

export const stickyMultibyteStateIso2022JpLoose = {
  test() {
    const vectors = [
      [[27], '\uFFFD'],
      [[27, 0x28], '\uFFFD('],
      [[0x1b, 0x28, 0x49], ''],
    ];

    const d = new TextDecoder('iso-2022-jp');
    for (const [bytes, text] of vectors) {
      strictEqual(d.decode(Uint8Array.of(0x40)), '@');
      strictEqual(d.decode(Uint8Array.from(bytes)), text);
      strictEqual(d.decode(Uint8Array.of(0x40)), '@');
      strictEqual(d.decode(Uint8Array.of(0x2a)), '*');
      strictEqual(d.decode(Uint8Array.of(0x42)), 'B');
    }
  },
};

export const fatalStreamGb18030Gbk = {
  test() {
    for (const encoding of ['gb18030', 'gbk']) {
      {
        const d = new TextDecoder(encoding, { fatal: true });
        strictEqual(d.decode(Uint8Array.of(0x80), { stream: true }), '\u20AC');
        throws(() =>
          d.decode(Uint8Array.of(0x81, 0x30, 0x21, 0x21, 0x21), {
            stream: true,
          })
        );
        strictEqual(d.decode(Uint8Array.of(0x80)), '\u20AC');
      }

      {
        const d = new TextDecoder(encoding, { fatal: true });
        strictEqual(d.decode(Uint8Array.of(0x80), { stream: true }), '\u20AC');
        throws(() =>
          d.decode(Uint8Array.of(0x81, 0x30, 0x81, 0x42, 0x42), {
            stream: true,
          })
        );
        strictEqual(d.decode(Uint8Array.of(0x80)), '\u20AC');
      }
    }
  },
};

export const textDecoderStream = {
  test() {
    const stream = new TextDecoderStream('utf-16', {
      fatal: true,
      ignoreBOM: true,
    });
    strictEqual(stream.encoding, 'utf-16le');
    strictEqual(stream.fatal, true);
    strictEqual(stream.ignoreBOM, true);

    const enc = new TextEncoderStream();
    strictEqual(enc.encoding, 'utf-8');
  },
};

// Per WHATWG Big5 decoder step 1, when end-of-queue is reached with a
// pending lead byte, the decoder must return error (U+FFFD in replacement
// mode, throw in fatal mode). This tests the streaming case where a lead
// byte is buffered in one call and then flushed without a trail byte.
export const big5OrphanedLeadOnFlush = {
  test() {
    // 0xA4 is a valid Big5 lead byte (e.g., first byte of 中 = 0xA4 0xA4).
    // Streaming it alone, then flushing, must produce U+FFFD.
    {
      const dec = new TextDecoder('big5');
      strictEqual(dec.decode(Uint8Array.of(0xa4), { stream: true }), '');
      strictEqual(dec.decode(), '\uFFFD');
    }

    // Fatal mode must throw on the orphaned lead.
    {
      const dec = new TextDecoder('big5', { fatal: true });
      strictEqual(dec.decode(Uint8Array.of(0xa4), { stream: true }), '');
      throws(() => dec.decode());
    }

    // Orphaned lead followed by an invalid trail byte on flush: the lead
    // must produce U+FFFD. 0x20 (space) is not a valid Big5 trail byte
    // (valid trails are 0x40-0x7E and 0xA1-0xFE).
    {
      const dec = new TextDecoder('big5');
      strictEqual(dec.decode(Uint8Array.of(0xa4), { stream: true }), '');
      const result = dec.decode(Uint8Array.of(0x20));
      // The orphaned lead must produce at least one U+FFFD.
      ok(
        result.includes('\uFFFD'),
        `expected U+FFFD in output, got: ${JSON.stringify(result)}`
      );
      // The space byte must not be swallowed.
      ok(
        result.includes(' '),
        `expected space in output, got: ${JSON.stringify(result)}`
      );
    }

    // Streaming a complete pair across two calls must still work.
    {
      const dec = new TextDecoder('big5');
      strictEqual(dec.decode(Uint8Array.of(0xa4), { stream: true }), '');
      strictEqual(dec.decode(Uint8Array.of(0xa4)), '中');
    }
  },
};

// Test x-user-defined encoding per WHATWG spec
// https://encoding.spec.whatwg.org/#x-user-defined-decoder
export const xUserDefinedDecode = {
  test() {
    const decoder = new TextDecoder('x-user-defined');
    strictEqual(decoder.encoding, 'x-user-defined');
    strictEqual(decoder.fatal, false);
    strictEqual(decoder.ignoreBOM, false);

    // Test ASCII bytes (0x00-0x7F) - identity mapping
    strictEqual(decoder.decode(Uint8Array.of(0x41)), 'A');
    strictEqual(decoder.decode(Uint8Array.of(0x00)), '\u0000');
    strictEqual(decoder.decode(Uint8Array.of(0x7f)), '\u007F');

    // Test high bytes (0x80-0xFF) - map to Private Use Area U+F780-U+F7FF
    strictEqual(decoder.decode(Uint8Array.of(0x80)), '\uF780');
    strictEqual(decoder.decode(Uint8Array.of(0x81)), '\uF781');
    strictEqual(decoder.decode(Uint8Array.of(0xff)), '\uF7FF');

    // Test mixed sequence
    const mixed = new Uint8Array([0x00, 0x7f, 0x80, 0x81, 0xff]);
    strictEqual(decoder.decode(mixed), '\u0000\u007F\uF780\uF781\uF7FF');

    // Test empty input
    strictEqual(decoder.decode(new Uint8Array([])), '');
    strictEqual(decoder.decode(), '');

    // Test pure ASCII input (fast path)
    strictEqual(
      decoder.decode(new Uint8Array([0x48, 0x65, 0x6c, 0x6c, 0x6f])),
      'Hello'
    );

    // Test streaming (x-user-defined is single-byte, streaming is trivial)
    const streamDecoder = new TextDecoder('x-user-defined');
    let result = '';
    result += streamDecoder.decode(Uint8Array.of(0x41), { stream: true });
    result += streamDecoder.decode(Uint8Array.of(0x80), { stream: true });
    result += streamDecoder.decode(Uint8Array.of(0xff), { stream: true });
    result += streamDecoder.decode();
    strictEqual(result, 'A\uF780\uF7FF');
  },
};

// Test x-user-defined with fatal option (all 256 bytes are valid)
export const xUserDefinedFatal = {
  test() {
    const decoder = new TextDecoder('x-user-defined', { fatal: true });
    strictEqual(decoder.fatal, true);

    // All 256 byte values are valid, fatal mode should never throw
    for (let byte = 0; byte < 256; byte++) {
      const decoded = decoder.decode(Uint8Array.of(byte));
      if (byte < 0x80) {
        strictEqual(decoded.codePointAt(0), byte);
      } else {
        strictEqual(decoded.codePointAt(0), 0xf700 + byte);
      }
    }
  },
};

// Verify that streaming with zero-length input works for every legacy
// encoding handled by the Rust LegacyDecoder. An empty chunk in streaming
// mode must produce an empty string and leave the decoder in a valid state
// for subsequent calls.
export const legacyStreamEmptyInput = {
  test() {
    const encodings = [
      'big5',
      'euc-jp',
      'euc-kr',
      'gb18030',
      'gbk',
      'iso-2022-jp',
      'shift_jis',
      'windows-1252',
      'x-user-defined',
    ];

    const empty = new Uint8Array(0);

    for (const label of encodings) {
      for (const fatal of [false, true]) {
        const dec = new TextDecoder(label, { fatal });

        // Empty stream chunk must produce empty string.
        strictEqual(
          dec.decode(empty, { stream: true }),
          '',
          `${label} (fatal=${fatal}): empty stream chunk should be ''`
        );

        // A second empty stream chunk must also be fine.
        strictEqual(
          dec.decode(empty, { stream: true }),
          '',
          `${label} (fatal=${fatal}): second empty stream chunk should be ''`
        );

        // Final flush with no pending bytes must produce empty string.
        strictEqual(
          dec.decode(),
          '',
          `${label} (fatal=${fatal}): flush after empty chunks should be ''`
        );

        // Decoder must still work normally after the empty-stream sequence.
        // Feed a single ASCII byte to verify.
        strictEqual(
          dec.decode(Uint8Array.of(0x41)),
          'A',
          `${label} (fatal=${fatal}): decode 'A' after empty stream should work`
        );
      }
    }
  },
};
