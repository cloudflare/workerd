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
          const control = new Array(bufferLength);
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
      ['866', 'ibm-866'],
      ['cp866', 'ibm-866'],
      ['csibm866', 'ibm-866'],
      ['ibm866', 'ibm-866'],
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
      ['csshiftjis', 'shift-jis'],
      ['ms932', 'shift-jis'],
      ['ms_kanji', 'shift-jis'],
      ['shift-jis', 'shift-jis'],
      ['shift_jis', 'shift-jis'],
      ['sjis', 'shift-jis'],
      ['windows-31j', 'shift-jis'],
      ['x-sjis', 'shift-jis'],
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
      ['x-user-defined', undefined],
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
