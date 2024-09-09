import assert from 'node:assert';
import { Buffer } from 'node:buffer';
import { Readable, Writable, Stream } from 'node:stream';
import { inspect, promisify } from 'node:util';
import { mock } from 'node:test';
import zlib from 'node:zlib';
import crypto from 'node:crypto';

// The following test data comes from
// https://github.com/zlib-ng/zlib-ng/blob/5401b24/test/test_crc32.cc
// test_crc32.cc -- crc32 unit test
// Copyright (C) 2019-2021 IBM Corporation
// Authors: Rogerio Alves    <rogealve@br.ibm.com>
//          Matheus Castanho <msc@linux.ibm.com>
// For conditions of distribution and use, see copyright notice in zlib.h
export const crc32Test = {
  test() {
    const tests = [
      [0x0, 0x0, 0, 0x0],
      [0xffffffff, 0x0, 0, 0x0],
      [0x0, 0x0, 255, 0x0] /*  BZ 174799.  */,
      [0x0, 0x0, 256, 0x0],
      [0x0, 0x0, 257, 0x0],
      [0x0, 0x0, 32767, 0x0],
      [0x0, 0x0, 32768, 0x0],
      [0x0, 0x0, 32769, 0x0],
      [0x0, '', 0, 0x0],
      [0xffffffff, '', 0, 0xffffffff],
      [0x0, 'abacus', 6, 0xc3d7115b],
      [0x0, 'backlog', 7, 0x269205],
      [0x0, 'campfire', 8, 0x22a515f8],
      [0x0, 'delta', 5, 0x9643fed9],
      [0x0, 'executable', 10, 0xd68eda01],
      [0x0, 'file', 4, 0x8c9f3610],
      [0x0, 'greatest', 8, 0xc1abd6cd],
      [0x0, 'hello', 5, 0x3610a686],
      [0x0, 'inverter', 8, 0xc9e962c9],
      [0x0, 'jigsaw', 6, 0xce4e3f69],
      [0x0, 'karate', 6, 0x890be0e2],
      [0x0, 'landscape', 9, 0xc4e0330b],
      [0x0, 'machine', 7, 0x1505df84],
      [0x0, 'nanometer', 9, 0xd4e19f39],
      [0x0, 'oblivion', 8, 0xdae9de77],
      [0x0, 'panama', 6, 0x66b8979c],
      [0x0, 'quest', 5, 0x4317f817],
      [0x0, 'resource', 8, 0xbc91f416],
      [0x0, 'secret', 6, 0x5ca2e8e5],
      [0x0, 'test', 4, 0xd87f7e0c],
      [0x0, 'ultimate', 8, 0x3fc79b0b],
      [0x0, 'vector', 6, 0x1b6e485b],
      [0x0, 'walrus', 6, 0xbe769b97],
      [0x0, 'xeno', 4, 0xe7a06444],
      [0x0, 'yelling', 7, 0xfe3944e5],
      [0x0, 'zlib', 4, 0x73887d3a],
      [0x0, '4BJD7PocN1VqX0jXVpWB', 20, 0xd487a5a1],
      [0x0, 'F1rPWI7XvDs6nAIRx41l', 20, 0x61a0132e],
      [0x0, 'ldhKlsVkPFOveXgkGtC2', 20, 0xdf02f76],
      [0x0, '5KKnGOOrs8BvJ35iKTOS', 20, 0x579b2b0a],
      [0x0, '0l1tw7GOcem06Ddu7yn4', 20, 0xf7d16e2d],
      [0x0, 'MCr47CjPIn9R1IvE1Tm5', 20, 0x731788f5],
      [0x0, 'UcixbzPKTIv0SvILHVdO', 20, 0x7112bb11],
      [0x0, 'dGnAyAhRQDsWw0ESou24', 20, 0xf32a0dac],
      [0x0, 'di0nvmY9UYMYDh0r45XT', 20, 0x625437bb],
      [0x0, '2XKDwHfAhFsV0RhbqtvH', 20, 0x896930f9],
      [0x0, 'ZhrANFIiIvRnqClIVyeD', 20, 0x8579a37],
      [0x0, 'v7Q9ehzioTOVeDIZioT1', 20, 0x632aa8e0],
      [0x0, 'Yod5hEeKcYqyhfXbhxj2', 20, 0xc829af29],
      [0x0, 'GehSWY2ay4uUKhehXYb0', 20, 0x1b08b7e8],
      [0x0, 'kwytJmq6UqpflV8Y8GoE', 20, 0x4e33b192],
      [0x0, '70684206568419061514', 20, 0x59a179f0],
      [0x0, '42015093765128581010', 20, 0xcd1013d7],
      [0x0, '88214814356148806939', 20, 0xab927546],
      [0x0, '43472694284527343838', 20, 0x11f3b20c],
      [0x0, '49769333513942933689', 20, 0xd562d4ca],
      [0x0, '54979784887993251199', 20, 0x233395f7],
      [0x0, '58360544869206793220', 20, 0x2d167fd5],
      [0x0, '27347953487840714234', 20, 0x8b5108ba],
      [0x0, '07650690295365319082', 20, 0xc46b3cd8],
      [0x0, '42655507906821911703', 20, 0xc10b2662],
      [0x0, '29977409200786225655', 20, 0xc9a0f9d2],
      [0x0, '85181542907229116674', 20, 0x9341357b],
      [0x0, '87963594337989416799', 20, 0xf0424937],
      [0x0, '21395988329504168551', 20, 0xd7c4c31f],
      [0x0, '51991013580943379423', 20, 0xf11edcc4],
      [0x0, '*]+@!);({_$;}[_},?{?;(_?,=-][@', 30, 0x40795df4],
      [0x0, '_@:_).&(#.[:[{[:)$++-($_;@[)}+', 30, 0xdd61a631],
      [0x0, '&[!,[$_==}+.]@!;*(+},[;:)$;)-@', 30, 0xca907a99],
      [0x0, ']{.[.+?+[[=;[?}_#&;[=)__$$:+=_', 30, 0xf652deac],
      [0x0, '-%.)=/[@].:.(:,()$;=%@-$?]{%+%', 30, 0xaf39a5a9],
      [0x0, '+]#$(@&.=:,*];/.!]%/{:){:@(;)$', 30, 0x6bebb4cf],
      // eslint-disable-next-line no-template-curly-in-string
      [0x0, ')-._.:?[&:.=+}(*$/=!.${;(=$@!}', 30, 0x76430bac],
      [0x0, ':(_*&%/[[}+,?#$&*+#[([*-/#;%(]', 30, 0x6c80c388],
      [0x0, '{[#-;:$/{)(+[}#]/{&!%(@)%:@-$:', 30, 0xd54d977d],
      [0x0, '_{$*,}(&,@.)):=!/%(&(,,-?$}}}!', 30, 0xe3966ad5],
      [
        0x0,
        'e$98KNzqaV)Y:2X?]77].{gKRD4G5{mHZk,Z)SpU%L3FSgv!Wb8MLAFdi{+fp)c,@8m6v)yXg@]HBDFk?.4&}g5_udE*JHCiH=aL',
        100,
        0xe7c71db9,
      ],
      [
        0x0,
        'r*Fd}ef+5RJQ;+W=4jTR9)R*p!B;]Ed7tkrLi;88U7g@3v!5pk2X6D)vt,.@N8c]@yyEcKi[vwUu@.Ppm@C6%Mv*3Nw}Y,58_aH)',
        100,
        0xeaa52777,
      ],
      [
        0x0,
        'h{bcmdC+a;t+Cf{6Y_dFq-{X4Yu&7uNfVDh?q&_u.UWJU],-GiH7ADzb7-V.Q%4=+v!$L9W+T=bP]$_:]Vyg}A.ygD.r;h-D]m%&',
        100,
        0xcd472048,
      ],
      [0x7a30360d, 'abacus', 6, 0xf8655a84],
      [0x6fd767ee, 'backlog', 7, 0x1ed834b1],
      [0xefeb7589, 'campfire', 8, 0x686cfca],
      [0x61cf7e6b, 'delta', 5, 0x1554e4b1],
      [0xdc712e2, 'executable', 10, 0x761b4254],
      [0xad23c7fd, 'file', 4, 0x7abdd09b],
      [0x85cb2317, 'greatest', 8, 0x4ba91c6b],
      [0x9eed31b0, 'inverter', 8, 0xd5e78ba5],
      [0xb94f34ca, 'jigsaw', 6, 0x23649109],
      [0xab058a2, 'karate', 6, 0xc5591f41],
      [0x5bff2b7a, 'landscape', 9, 0xf10eb644],
      [0x605c9a5f, 'machine', 7, 0xbaa0a636],
      [0x51bdeea5, 'nanometer', 9, 0x6af89afb],
      [0x85c21c79, 'oblivion', 8, 0xecae222b],
      [0x97216f56, 'panama', 6, 0x47dffac4],
      [0x18444af2, 'quest', 5, 0x70c2fe36],
      [0xbe6ce359, 'resource', 8, 0x1471d925],
      [0x843071f1, 'secret', 6, 0x50c9a0db],
      [0xf2480c60, 'ultimate', 8, 0xf973daf8],
      [0x2d2feb3d, 'vector', 6, 0x344ac03d],
      [0x7490310a, 'walrus', 6, 0x6d1408ef],
      [0x97d247d4, 'xeno', 4, 0xe62670b5],
      [0x93cf7599, 'yelling', 7, 0x1b36da38],
      [0x73c84278, 'zlib', 4, 0x6432d127],
      [0x228a87d1, '4BJD7PocN1VqX0jXVpWB', 20, 0x997107d0],
      [0xa7a048d0, 'F1rPWI7XvDs6nAIRx41l', 20, 0xdc567274],
      [0x1f0ded40, 'ldhKlsVkPFOveXgkGtC2', 20, 0xdcc63870],
      [0xa804a62f, '5KKnGOOrs8BvJ35iKTOS', 20, 0x6926cffd],
      [0x508fae6a, '0l1tw7GOcem06Ddu7yn4', 20, 0xb52b38bc],
      [0xe5adaf4f, 'MCr47CjPIn9R1IvE1Tm5', 20, 0xf83b8178],
      [0x67136a40, 'UcixbzPKTIv0SvILHVdO', 20, 0xc5213070],
      [0xb00c4a10, 'dGnAyAhRQDsWw0ESou24', 20, 0xbc7648b0],
      [0x2e0c84b5, 'di0nvmY9UYMYDh0r45XT', 20, 0xd8123a72],
      [0x81238d44, '2XKDwHfAhFsV0RhbqtvH', 20, 0xd5ac5620],
      [0xf853aa92, 'ZhrANFIiIvRnqClIVyeD', 20, 0xceae099d],
      [0x5a692325, 'v7Q9ehzioTOVeDIZioT1', 20, 0xb07d2b24],
      [0x3275b9f, 'Yod5hEeKcYqyhfXbhxj2', 20, 0x24ce91df],
      [0x38371feb, 'GehSWY2ay4uUKhehXYb0', 20, 0x707b3b30],
      [0xafc8bf62, 'kwytJmq6UqpflV8Y8GoE', 20, 0x16abc6a9],
      [0x9b07db73, '70684206568419061514', 20, 0xae1fb7b7],
      [0xe75b214, '42015093765128581010', 20, 0xd4eecd2d],
      [0x72d0fe6f, '88214814356148806939', 20, 0x4660ec7],
      [0xf857a4b1, '43472694284527343838', 20, 0xfd8afdf7],
      [0x54b8e14, '49769333513942933689', 20, 0xc6d1b5f2],
      [0xd6aa5616, '54979784887993251199', 20, 0x32476461],
      [0x11e63098, '58360544869206793220', 20, 0xd917cf1a],
      [0xbe92385, '27347953487840714234', 20, 0x4ad14a12],
      [0x49511de0, '07650690295365319082', 20, 0xe37b5c6c],
      [0x3db13bc1, '42655507906821911703', 20, 0x7cc497f1],
      [0xbb899bea, '29977409200786225655', 20, 0x99781bb2],
      [0xf6cd9436, '85181542907229116674', 20, 0x132256a1],
      [0x9109e6c3, '87963594337989416799', 20, 0xbfdb2c83],
      [0x75770fc, '21395988329504168551', 20, 0x8d9d1e81],
      [0x69b1d19b, '51991013580943379423', 20, 0x7b6d4404],
      [0xc6132975, '*]+@!);({_$;}[_},?{?;(_?,=-][@', 30, 0x8619f010],
      [0xd58cb00c, '_@:_).&(#.[:[{[:)$++-($_;@[)}+', 30, 0x15746ac3],
      [0xb63b8caa, '&[!,[$_==}+.]@!;*(+},[;:)$;)-@', 30, 0xaccf812f],
      [0x8a45a2b8, ']{.[.+?+[[=;[?}_#&;[=)__$$:+=_', 30, 0x78af45de],
      [0xcbe95b78, '-%.)=/[@].:.(:,()$;=%@-$?]{%+%', 30, 0x25b06b59],
      [0x4ef8a54b, '+]#$(@&.=:,*];/.!]%/{:){:@(;)$', 30, 0x4ba0d08f],
      // eslint-disable-next-line no-template-curly-in-string
      [0x76ad267a, ')-._.:?[&:.=+}(*$/=!.${;(=$@!}', 30, 0xe26b6aac],
      [0x569e613c, ':(_*&%/[[}+,?#$&*+#[([*-/#;%(]', 30, 0x7e2b0a66],
      [0x36aa61da, '{[#-;:$/{)(+[}#]/{&!%(@)%:@-$:', 30, 0xb3430dc7],
      [0xf67222df, '_{$*,}(&,@.)):=!/%(&(,,-?$}}}!', 30, 0x626c17a],
      [
        0x74b34fd3,
        'e$98KNzqaV)Y:2X?]77].{gKRD4G5{mHZk,Z)SpU%L3FSgv!Wb8MLAFdi{+fp)c,@8m6v)yXg@]HBDFk?.4&}g5_udE*JHCiH=aL',
        100,
        0xccf98060,
      ],
      [
        0x351fd770,
        'r*Fd}ef+5RJQ;+W=4jTR9)R*p!B;]Ed7tkrLi;88U7g@3v!5pk2X6D)vt,.@N8c]@yyEcKi[vwUu@.Ppm@C6%Mv*3Nw}Y,58_aH)',
        100,
        0xd8b95312,
      ],
      [
        0xc45aef77,
        'h{bcmdC+a;t+Cf{6Y_dFq-{X4Yu&7uNfVDh?q&_u.UWJU],-GiH7ADzb7-V.Q%4=+v!$L9W+T=bP]$_:]Vyg}A.ygD.r;h-D]m%&',
        100,
        0xbb1c9912,
      ],
      [
        0xc45aef77,
        'h{bcmdC+a;t+Cf{6Y_dFq-{X4Yu&7uNfVDh?q&_u.UWJU],-GiH7ADzb7-V.Q%4=+v!$L9W+T=bP]$_:]Vyg}A.ygD.r;h-D]m%&' +
          'h{bcmdC+a;t+Cf{6Y_dFq-{X4Yu&7uNfVDh?q&_u.UWJU],-GiH7ADzb7-V.Q%4=+v!$L9W+T=bP]$_:]Vyg}A.ygD.r;h-D]m%&' +
          'h{bcmdC+a;t+Cf{6Y_dFq-{X4Yu&7uNfVDh?q&_u.UWJU],-GiH7ADzb7-V.Q%4=+v!$L9W+T=bP]$_:]Vyg}A.ygD.r;h-D]m%&' +
          'h{bcmdC+a;t+Cf{6Y_dFq-{X4Yu&7uNfVDh?q&_u.UWJU],-GiH7ADzb7-V.Q%4=+v!$L9W+T=bP]$_:]Vyg}A.ygD.r;h-D]m%&' +
          'h{bcmdC+a;t+Cf{6Y_dFq-{X4Yu&7uNfVDh?q&_u.UWJU],-GiH7ADzb7-V.Q%4=+v!$L9W+T=bP]$_:]Vyg}A.ygD.r;h-D]m%&' +
          'h{bcmdC+a;t+Cf{6Y_dFq-{X4Yu&7uNfVDh?q&_u.UWJU],-GiH7ADzb7-V.Q%4=+v!$L9W+T=bP]$_:]Vyg}A.ygD.r;h-D]m%&',
        600,
        0x888afa5b,
      ],
    ];

    for (const [crc, data, len, expected] of tests) {
      if (data === 0) {
        continue;
      }
      const buf = Buffer.from(data, 'utf8');
      assert.strictEqual(buf.length, len);
      assert.strictEqual(
        zlib.crc32(buf, crc),
        expected,
        `crc32('${data}', ${crc}) in buffer is not ${expected}`
      );
      assert.strictEqual(
        zlib.crc32(buf.toString(), crc),
        expected,
        `crc32('${data}', ${crc}) in string is not ${expected}`
      );
      if (crc === 0) {
        assert.strictEqual(
          zlib.crc32(buf),
          expected,
          `crc32('${data}') in buffer is not ${expected}`
        );
        assert.strictEqual(
          zlib.crc32(buf.toString()),
          expected,
          `crc32('${data}') in string is not ${expected}`
        );
      }
    }

    for (const invalid of [undefined, null, true, 1, () => {}, {}]) {
      assert.throws(() => {
        zlib.crc32(invalid);
      }, new TypeError("Failed to execute 'crc32' on 'ZlibUtil': parameter 1 is not of type 'string or ArrayBuffer or ArrayBufferView'."));
    }

    for (const invalid of [null, true, () => {}, {}]) {
      assert.throws(
        () => {
          zlib.crc32('test', invalid);
        },
        { code: 'ERR_INVALID_ARG_TYPE' }
      );
    }
  },
};

export const constantsTest = {
  test() {
    assert.deepStrictEqual(Object.keys(zlib.constants).sort(), [
      'BROTLI_DECODE',
      'BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES',
      'BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP',
      'BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES',
      'BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1',
      'BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2',
      'BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS',
      'BROTLI_DECODER_ERROR_DICTIONARY_NOT_SET',
      'BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_1',
      'BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_2',
      'BROTLI_DECODER_ERROR_FORMAT_CL_SPACE',
      'BROTLI_DECODER_ERROR_FORMAT_CONTEXT_MAP_REPEAT',
      'BROTLI_DECODER_ERROR_FORMAT_DICTIONARY',
      'BROTLI_DECODER_ERROR_FORMAT_DISTANCE',
      'BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_META_NIBBLE',
      'BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_NIBBLE',
      'BROTLI_DECODER_ERROR_FORMAT_HUFFMAN_SPACE',
      'BROTLI_DECODER_ERROR_FORMAT_PADDING_1',
      'BROTLI_DECODER_ERROR_FORMAT_PADDING_2',
      'BROTLI_DECODER_ERROR_FORMAT_RESERVED',
      'BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_ALPHABET',
      'BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_SAME',
      'BROTLI_DECODER_ERROR_FORMAT_TRANSFORM',
      'BROTLI_DECODER_ERROR_FORMAT_WINDOW_BITS',
      'BROTLI_DECODER_ERROR_INVALID_ARGUMENTS',
      'BROTLI_DECODER_ERROR_UNREACHABLE',
      'BROTLI_DECODER_NEEDS_MORE_INPUT',
      'BROTLI_DECODER_NEEDS_MORE_OUTPUT',
      'BROTLI_DECODER_NO_ERROR',
      'BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION',
      'BROTLI_DECODER_PARAM_LARGE_WINDOW',
      'BROTLI_DECODER_RESULT_ERROR',
      'BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT',
      'BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT',
      'BROTLI_DECODER_RESULT_SUCCESS',
      'BROTLI_DECODER_SUCCESS',
      'BROTLI_DEFAULT_MODE',
      'BROTLI_DEFAULT_QUALITY',
      'BROTLI_DEFAULT_WINDOW',
      'BROTLI_ENCODE',
      'BROTLI_LARGE_MAX_WINDOW_BITS',
      'BROTLI_MAX_INPUT_BLOCK_BITS',
      'BROTLI_MAX_QUALITY',
      'BROTLI_MAX_WINDOW_BITS',
      'BROTLI_MIN_INPUT_BLOCK_BITS',
      'BROTLI_MIN_QUALITY',
      'BROTLI_MIN_WINDOW_BITS',
      'BROTLI_MODE_FONT',
      'BROTLI_MODE_GENERIC',
      'BROTLI_MODE_TEXT',
      'BROTLI_OPERATION_EMIT_METADATA',
      'BROTLI_OPERATION_FINISH',
      'BROTLI_OPERATION_FLUSH',
      'BROTLI_OPERATION_PROCESS',
      'BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING',
      'BROTLI_PARAM_LARGE_WINDOW',
      'BROTLI_PARAM_LGBLOCK',
      'BROTLI_PARAM_LGWIN',
      'BROTLI_PARAM_MODE',
      'BROTLI_PARAM_NDIRECT',
      'BROTLI_PARAM_NPOSTFIX',
      'BROTLI_PARAM_QUALITY',
      'BROTLI_PARAM_SIZE_HINT',
      'DEFLATE',
      'DEFLATERAW',
      'GUNZIP',
      'GZIP',
      'INFLATE',
      'INFLATERAW',
      'UNZIP',
      'ZLIB_VERNUM',
      'Z_BEST_COMPRESSION',
      'Z_BEST_SPEED',
      'Z_BLOCK',
      'Z_BUF_ERROR',
      'Z_DATA_ERROR',
      'Z_DEFAULT_CHUNK',
      'Z_DEFAULT_COMPRESSION',
      'Z_DEFAULT_LEVEL',
      'Z_DEFAULT_MEMLEVEL',
      'Z_DEFAULT_STRATEGY',
      'Z_DEFAULT_WINDOWBITS',
      'Z_ERRNO',
      'Z_FILTERED',
      'Z_FINISH',
      'Z_FIXED',
      'Z_FULL_FLUSH',
      'Z_HUFFMAN_ONLY',
      'Z_MAX_CHUNK',
      'Z_MAX_LEVEL',
      'Z_MAX_MEMLEVEL',
      'Z_MAX_WINDOWBITS',
      'Z_MEM_ERROR',
      'Z_MIN_CHUNK',
      'Z_MIN_LEVEL',
      'Z_MIN_MEMLEVEL',
      'Z_MIN_WINDOWBITS',
      'Z_NEED_DICT',
      'Z_NO_COMPRESSION',
      'Z_NO_FLUSH',
      'Z_OK',
      'Z_PARTIAL_FLUSH',
      'Z_RLE',
      'Z_STREAM_END',
      'Z_STREAM_ERROR',
      'Z_SYNC_FLUSH',
      'Z_VERSION_ERROR',
    ]);
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/561bc87c7607208f0d3db6dcd9231efeb48cfe2f/test/parallel/test-zlib-zero-windowBits.js
export const testZeroWindowBits = {
  test() {
    // windowBits is a special case in zlib. On the compression side, 0 is invalid.
    // On the decompression side, it indicates that zlib should use the value from
    // the header of the compressed stream.
    {
      const inflate = zlib.createInflate({ windowBits: 0 });
      assert(inflate instanceof zlib.Inflate);
    }

    {
      const gunzip = zlib.createGunzip({ windowBits: 0 });
      assert(gunzip instanceof zlib.Gunzip);
    }

    {
      const unzip = zlib.createUnzip({ windowBits: 0 });
      assert(unzip instanceof zlib.Unzip);
    }

    assert.throws(() => zlib.createGzip({ windowBits: 0 }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
      message:
        'The value of "options.windowBits" is out of range. ' +
        'It must be >= 9 and <= 15. Received 0',
    });
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/561bc87c7607208f0d3db6dcd9231efeb48cfe2f/test/parallel/test-zlib-create-raw.js
export const testCreateRaw = {
  test() {
    {
      const inflateRaw = zlib.createInflateRaw();
      assert(inflateRaw instanceof zlib.InflateRaw);
    }

    {
      const deflateRaw = zlib.createDeflateRaw();
      assert(deflateRaw instanceof zlib.DeflateRaw);
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/561bc87c7607208f0d3db6dcd9231efeb48cfe2f/test/parallel/test-zlib-deflate-constructors.js
export const testDeflateConstructors = {
  test() {
    assert(new zlib.Deflate() instanceof zlib.Deflate);
    assert(new zlib.DeflateRaw() instanceof zlib.DeflateRaw);

    // Throws if `options.chunkSize` is invalid
    assert.throws(() => new zlib.Deflate({ chunkSize: 'test' }), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });

    assert.throws(() => new zlib.Deflate({ chunkSize: -Infinity }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate({ chunkSize: 0 }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    // Throws if `options.windowBits` is invalid
    assert.throws(() => new zlib.Deflate({ windowBits: 'test' }), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });

    assert.throws(() => new zlib.Deflate({ windowBits: -Infinity }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate({ windowBits: Infinity }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate({ windowBits: 0 }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    // Throws if `options.level` is invalid
    assert.throws(() => new zlib.Deflate({ level: 'test' }), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });

    assert.throws(() => new zlib.Deflate({ level: -Infinity }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate({ level: Infinity }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate({ level: -2 }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    // Throws if `level` invalid in  `Deflate.prototype.params()`
    assert.throws(() => new zlib.Deflate().params('test'), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });

    assert.throws(() => new zlib.Deflate().params(-Infinity), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate().params(Infinity), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate().params(-2), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    // Throws if options.memLevel is invalid
    assert.throws(() => new zlib.Deflate({ memLevel: 'test' }), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });

    assert.throws(() => new zlib.Deflate({ memLevel: -Infinity }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate({ memLevel: Infinity }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate({ memLevel: -2 }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    // Does not throw if opts.strategy is valid
    new zlib.Deflate({ strategy: zlib.constants.Z_FILTERED });
    new zlib.Deflate({ strategy: zlib.constants.Z_HUFFMAN_ONLY });
    new zlib.Deflate({ strategy: zlib.constants.Z_RLE });
    new zlib.Deflate({ strategy: zlib.constants.Z_FIXED });
    new zlib.Deflate({ strategy: zlib.constants.Z_DEFAULT_STRATEGY });

    // Throws if options.strategy is invalid
    assert.throws(() => new zlib.Deflate({ strategy: 'test' }), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });

    assert.throws(() => new zlib.Deflate({ strategy: -Infinity }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate({ strategy: Infinity }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate({ strategy: -2 }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    // Throws TypeError if `strategy` is invalid in `Deflate.prototype.params()`
    assert.throws(() => new zlib.Deflate().params(0, 'test'), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });

    assert.throws(() => new zlib.Deflate().params(0, -Infinity), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate().params(0, Infinity), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => new zlib.Deflate().params(0, -2), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    // Throws if opts.dictionary is not a Buffer
    assert.throws(() => new zlib.Deflate({ dictionary: 'not a buffer' }), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/561bc87c7607208f0d3db6dcd9231efeb48cfe2f/test/parallel/test-zlib-failed-init.js
export const testFailedInit = {
  test() {
    assert.throws(() => zlib.createGzip({ chunkSize: 0 }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => zlib.createGzip({ windowBits: 0 }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    assert.throws(() => zlib.createGzip({ memLevel: 0 }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    });

    {
      const stream = zlib.createGzip({ level: NaN });
      assert.strictEqual(stream._level, zlib.constants.Z_DEFAULT_COMPRESSION);
    }

    {
      const stream = zlib.createGzip({ strategy: NaN });
      assert.strictEqual(stream._strategy, zlib.constants.Z_DEFAULT_STRATEGY);
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/561bc87c7607208f0d3db6dcd9231efeb48cfe2f/test/parallel/test-zlib-destroy.js
export const zlibDestroyTest = {
  async test() {
    const promises = [];
    // Verify that the zlib transform does clean up
    // the handle when calling destroy.
    {
      const ts = zlib.createGzip();
      ts.destroy();
      assert.strictEqual(ts._handle, null);

      const { promise, resolve, reject } = Promise.withResolvers();
      promises.push(promise);
      ts.on('error', reject);
      ts.on('close', () => {
        ts.close(() => {
          resolve();
        });
      });
    }

    {
      // Ensure 'error' is only emitted once.
      const decompress = zlib.createGunzip(15);
      const { promise, resolve, reject } = Promise.withResolvers();
      promises.push(promise);
      let errorCount = 0;
      decompress.on('error', (err) => {
        errorCount++;
        decompress.close();
        assert.strictEqual(errorCount, 1, 'Error should only be emitted once');
        resolve();
      });

      decompress.write('something invalid');
      decompress.destroy(new Error('asd'));
    }

    await Promise.all(promises);
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/561bc87c7607208f0d3db6dcd9231efeb48cfe2f/test/parallel/test-zlib-close-after-error.js
export const closeAfterError = {
  async test() {
    const decompress = zlib.createGunzip(15);
    const { promise, resolve } = Promise.withResolvers();
    let errorHasBeenCalled = false;

    decompress.on('error', () => {
      errorHasBeenCalled = true;
      assert.strictEqual(decompress._closed, true);
      decompress.close();
    });

    assert.strictEqual(decompress._closed, false);
    decompress.write('something invalid');
    decompress.on('close', resolve);

    await promise;

    assert(errorHasBeenCalled, 'Error handler should have been called');
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/561bc87c7607208f0d3db6dcd9231efeb48cfe2f/test/parallel/test-zlib-write-after-close.js
export const writeAfterClose = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    let close = mock.fn();
    zlib.gzip('hello', function (err, out) {
      const unzip = zlib.createGunzip();
      unzip.close(close);
      unzip.write('asd', (err) => {
        assert.strictEqual(err.code, 'ERR_STREAM_DESTROYED');
        assert.strictEqual(err.name, 'Error');
        resolve();
      });
    });
    await promise;
    assert.strictEqual(close.mock.callCount(), 1);
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/9edf4a0856681a7665bd9dcf2ca7cac252784b98/test/parallel/test-zlib-bytes-read.js
export const testZlibBytesRead = {
  async test() {
    const expectStr = 'abcdefghijklmnopqrstuvwxyz'.repeat(2);
    const expectBuf = Buffer.from(expectStr);

    function createWriter(target, buffer) {
      const writer = { size: 0 };
      const write = () => {
        target.write(Buffer.from([buffer[writer.size++]]), () => {
          if (writer.size < buffer.length) {
            target.flush(write);
          } else {
            target.end();
          }
        });
      };
      write();
      return writer;
    }

    // This test is simplified a lot because of test runner limitations.
    for (const method of [
      'createGzip',
      'createDeflate',
      'createDeflateRaw',
      'createBrotliCompress',
    ]) {
      assert(method in zlib, `${method} is not available in "node:zlib"`);
      const { promise, resolve, reject } = Promise.withResolvers();
      let compData = Buffer.alloc(0);
      const comp = zlib[method]();
      const compWriter = createWriter(comp, expectBuf);
      comp.on('data', function (d) {
        compData = Buffer.concat([compData, d]);
        assert.strictEqual(
          this.bytesWritten,
          compWriter.size,
          `Should get write size on ${method} data.`
        );
      });
      comp.on('error', reject);
      comp.on('end', function () {
        assert.strictEqual(
          this.bytesWritten,
          compWriter.size,
          `Should get write size on ${method} end.`
        );
        assert.strictEqual(
          this.bytesWritten,
          expectStr.length,
          `Should get data size on ${method} end.`
        );

        resolve();
      });

      await promise;
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/3a71ccf6c473357e89be61b26739fd9139dce4db/test/parallel/test-zlib-const.js
export const zlibConst = {
  test() {
    assert.strictEqual(zlib.constants.Z_OK, 0, 'Expected Z_OK to be 0');
    assert.throws(() => {
      zlib.constants.Z_OK = 1;
    }, /Cannot assign to read only property/);
    assert.strictEqual(zlib.constants.Z_OK, 0, 'Z_OK should be immutable');
    assert.strictEqual(
      zlib.codes.Z_OK,
      0,
      `Expected Z_OK to be 0; got ${zlib.codes.Z_OK}`
    );
    assert.throws(() => {
      zlib.codes.Z_OK = 1;
    }, /Cannot assign to read only property/);
    assert.strictEqual(zlib.codes.Z_OK, 0, 'Z_OK should be immutable');
    assert(Object.isFrozen(zlib.codes), 'Expected zlib.codes to be frozen');

    assert.deepStrictEqual(zlib.codes, {
      '-1': 'Z_ERRNO',
      '-2': 'Z_STREAM_ERROR',
      '-3': 'Z_DATA_ERROR',
      '-4': 'Z_MEM_ERROR',
      '-5': 'Z_BUF_ERROR',
      '-6': 'Z_VERSION_ERROR',
      0: 'Z_OK',
      1: 'Z_STREAM_END',
      2: 'Z_NEED_DICT',
      Z_BUF_ERROR: -5,
      Z_DATA_ERROR: -3,
      Z_ERRNO: -1,
      Z_MEM_ERROR: -4,
      Z_NEED_DICT: 2,
      Z_OK: 0,
      Z_STREAM_END: 1,
      Z_STREAM_ERROR: -2,
      Z_VERSION_ERROR: -6,
    });
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/3a71ccf6c473357e89be61b26739fd9139dce4db/test/parallel/test-zlib-object-write.js

export const zlibObjectWrite = {
  async test() {
    const { promise, resolve, reject } = Promise.withResolvers();
    const gunzip = new zlib.Gunzip({ objectMode: true });
    gunzip.on('error', reject);
    assert.throws(
      () => {
        gunzip.write({});
      },
      {
        name: 'TypeError',
        code: 'ERR_INVALID_ARG_TYPE',
      }
    );
    gunzip.on('close', resolve);
    gunzip.close();
    await promise;
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/3a71ccf6c473357e89be61b26739fd9139dce4db/test/parallel/test-zlib-zero-byte.js
export const zlibZeroByte = {
  async test() {
    for (const Compressor of [zlib.Gzip, zlib.BrotliCompress]) {
      const { promise, resolve, reject } = Promise.withResolvers();
      let endCalled = false;
      const gz = new Compressor();
      const emptyBuffer = Buffer.alloc(0);
      let received = 0;
      gz.on('data', function (c) {
        received += c.length;
      });

      gz.on('end', function () {
        const expected = Compressor === zlib.Gzip ? 20 : 1;
        assert.strictEqual(
          received,
          expected,
          `${received}, ${expected}, ${Compressor.name}`
        );
        endCalled = true;
      });
      gz.on('error', reject);
      gz.on('finish', resolve);
      gz.write(emptyBuffer);
      gz.end();

      await promise;
      assert(endCalled, 'End should have been called');
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/431ac161e65698152de197703fb30c89da2b6686/test/parallel/test-zlib-dictionary-fail.js
export const zlibDictionaryFail = {
  async test() {
    // String "test" encoded with dictionary "dict".
    const input = Buffer.from([0x78, 0xbb, 0x04, 0x09, 0x01, 0xa5]);

    {
      const { promise, resolve } = Promise.withResolvers();
      const stream = zlib.createInflate();

      stream.on('error', function (err) {
        assert.match(err.message, /Missing dictionary/);
        resolve();
      });

      stream.write(input);
      await promise;
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      const stream = zlib.createInflate({ dictionary: Buffer.from('fail') });

      stream.on('error', function (err) {
        assert.match(err.message, /Bad dictionary/);
        resolve();
      });

      stream.write(input);
      await promise;
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      const stream = zlib.createInflateRaw({ dictionary: Buffer.from('fail') });

      stream.on('error', function (err) {
        // It's not possible to separate invalid dict and invalid data when using
        // the raw format
        assert.match(
          err.message,
          /(invalid|Operation-Ending-Supplemental Code is 0x12)/
        );
        resolve();
      });

      stream.write(input);
      await promise;
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/431ac161e65698152de197703fb30c89da2b6686/test/parallel/test-zlib-close-in-ondata.js
export const zlibCloseInOnData = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const ts = zlib.createGzip();
    const buf = Buffer.alloc(1024 * 1024 * 20);

    ts.on('data', function () {
      ts.close();
      resolve();
    });
    ts.end(buf);
    await promise;
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/431ac161e65698152de197703fb30c89da2b6686/test/parallel/test-zlib-flush-flags.js
export const zlibFlushFlags = {
  test() {
    zlib.createGzip({ flush: zlib.constants.Z_SYNC_FLUSH });

    assert.throws(() => zlib.createGzip({ flush: 'foobar' }), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
      message:
        'The "options.flush" property must be of type number. ' +
        "Received type string ('foobar')",
    });

    assert.throws(() => zlib.createGzip({ flush: 10000 }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
      message:
        'The value of "options.flush" is out of range. It must ' +
        'be >= 0 and <= 5. Received 10000',
    });

    zlib.createGzip({ finishFlush: zlib.constants.Z_SYNC_FLUSH });

    assert.throws(() => zlib.createGzip({ finishFlush: 'foobar' }), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
      message:
        'The "options.finishFlush" property must be of type number. ' +
        "Received type string ('foobar')",
    });

    assert.throws(() => zlib.createGzip({ finishFlush: 10000 }), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
      message:
        'The value of "options.finishFlush" is out of range. It must ' +
        'be >= 0 and <= 5. Received 10000',
    });
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/431ac161e65698152de197703fb30c89da2b6686/test/parallel/test-zlib-reset-before-write.js
export const zlibResetBeforeWrite = {
  async test() {
    // Tests that zlib streams support .reset() and .params()
    // before the first write. That is important to ensure that
    // lazy init of zlib native library handles these cases.

    for (const fn of [
      (z, cb) => {
        z.reset();
        cb();
      },
      (z, cb) => z.params(0, zlib.constants.Z_DEFAULT_STRATEGY, cb),
    ]) {
      const { promise, resolve, reject } = Promise.withResolvers();
      const deflate = zlib.createDeflate();
      const inflate = zlib.createInflate();

      deflate.pipe(inflate);

      const output = [];
      inflate
        .on('error', reject)
        .on('data', (chunk) => output.push(chunk))
        .on('end', resolve);

      fn(deflate, () => {
        fn(inflate, () => {
          deflate.write('abc');
          deflate.end();
        });
      });

      await promise;

      assert.strictEqual(Buffer.concat(output).toString(), 'abc');
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/6bf7b6e342f97cf48319e0bc251200fabe132c21/test/parallel/test-zlib-invalid-input.js
export const zlibInvalidInput = {
  async test() {
    const nonStringInputs = [1, true, { a: 1 }, ['a']];

    // zlib.Unzip classes need to get valid data, or else they'll throw.
    const unzips = [
      new zlib.Unzip(),
      new zlib.Gunzip(),
      new zlib.Inflate(),
      new zlib.InflateRaw(),
      new zlib.BrotliDecompress(),
    ];

    for (const input of nonStringInputs) {
      assert.throws(
        () => {
          zlib.gunzip(input);
        },
        {
          name: 'TypeError',
        }
      );
    }

    for (const uz of unzips) {
      const { promise, resolve, reject } = Promise.withResolvers();
      uz.on('error', resolve);
      uz.on('end', reject);

      // This will trigger error event
      uz.write('this is not valid compressed data.');
      await promise;
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/5e6aab0ecad6394e538e06357d6e16e155951a8b/test/parallel/test-zlib-unzip-one-byte-chunks.js
export const zlibUnzipOneByteChunks = {
  async test() {
    const { promise, resolve, reject } = Promise.withResolvers();
    const data = Buffer.concat([zlib.gzipSync('abc'), zlib.gzipSync('def')]);

    const resultBuffers = [];

    const unzip = zlib
      .createUnzip()
      .on('error', reject)
      .on('data', (data) => resultBuffers.push(data))
      .on('finish', resolve);

    for (let i = 0; i < data.length; i++) {
      // Write each single byte individually.
      unzip.write(Buffer.from([data[i]]));
    }

    unzip.end();
    await promise;
    assert.strictEqual(Buffer.concat(resultBuffers).toString(), 'abcdef');
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/4f14eb15454b9f6ae7f0145947debd2c79a2a84f/test/parallel/test-zlib-truncated.js
export const zlibTruncated = {
  async test() {
    const inputString =
      'ΩΩLorem ipsum dolor sit amet, consectetur adipiscing eli' +
      't. Morbi faucibus, purus at gravida dictum, libero arcu ' +
      'convallis lacus, in commodo libero metus eu nisi. Nullam' +
      ' commodo, neque nec porta placerat, nisi est fermentum a' +
      'ugue, vitae gravida tellus sapien sit amet tellus. Aenea' +
      'n non diam orci. Proin quis elit turpis. Suspendisse non' +
      ' diam ipsum. Suspendisse nec ullamcorper odio. Vestibulu' +
      'm arcu mi, sodales non suscipit id, ultrices ut massa. S' +
      'ed ac sem sit amet arcu malesuada fermentum. Nunc sed. ';

    const errMessage = /unexpected end of file/;

    const cases = [
      { comp: 'gzip', decomp: 'gunzip', decompSync: 'gunzipSync' },
      { comp: 'gzip', decomp: 'unzip', decompSync: 'unzipSync' },
      { comp: 'deflate', decomp: 'inflate', decompSync: 'inflateSync' },
      {
        comp: 'deflateRaw',
        decomp: 'inflateRaw',
        decompSync: 'inflateRawSync',
      },
    ];
    for (const methods of cases) {
      const { promise, resolve } = Promise.withResolvers();
      zlib[methods.comp](inputString, async function (err, compressed) {
        assert.ifError(err);
        const truncated = compressed.slice(0, compressed.length / 2);
        const toUTF8 = (buffer) => buffer.toString('utf-8');

        // sync sanity
        const decompressed = zlib[methods.decompSync](compressed);
        assert.strictEqual(toUTF8(decompressed), inputString);

        // async sanity
        {
          const { promise, resolve } = Promise.withResolvers();
          zlib[methods.decomp](compressed, function (err, result) {
            assert.ifError(err);
            assert.strictEqual(toUTF8(result), inputString);
            resolve();
          });

          await promise;
        }

        // Sync truncated input test
        assert.throws(function () {
          zlib[methods.decompSync](truncated);
        }, errMessage);

        // Async truncated input test
        {
          const { promise, resolve } = Promise.withResolvers();
          zlib[methods.decomp](truncated, function (err) {
            assert.match(err.message, errMessage);
            resolve();
          });

          await promise;
        }

        const syncFlushOpt = { finishFlush: zlib.constants.Z_SYNC_FLUSH };

        // Sync truncated input test, finishFlush = Z_SYNC_FLUSH
        const result = toUTF8(
          zlib[methods.decompSync](truncated, syncFlushOpt)
        );
        assert.strictEqual(result, inputString.slice(0, result.length));

        // Async truncated input test, finishFlush = Z_SYNC_FLUSH
        {
          const { promise, resolve } = Promise.withResolvers();
          zlib[methods.decomp](
            truncated,
            syncFlushOpt,
            function (err, decompressed) {
              assert.ifError(err);
              const result = toUTF8(decompressed);
              assert.strictEqual(result, inputString.slice(0, result.length));
              resolve();
            }
          );
          await promise;
        }

        resolve();
      });

      await promise;
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/beabcec41ca456e7e895e276acbc5f2d93db032f/test/parallel/test-zlib-from-concatenated-gzip.js
export const zlibFromConcatenatedGzip = {
  async test() {
    const abc = 'abc';
    const def = 'def';

    const abcEncoded = zlib.gzipSync(abc);
    const defEncoded = zlib.gzipSync(def);

    const data = Buffer.concat([abcEncoded, defEncoded]);

    assert.strictEqual(zlib.gunzipSync(data).toString(), abc + def);

    {
      const { promise, resolve } = Promise.withResolvers();
      zlib.gunzip(data, (err, result) => {
        assert.ifError(err);
        assert.strictEqual(result.toString(), abc + def);
        resolve();
      });

      await promise;
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      zlib.unzip(data, (err, result) => {
        assert.ifError(err);
        assert.strictEqual(result.toString(), abc + def);
        resolve();
      });

      await promise;
    }

    // Multi-member support does not apply to zlib inflate/deflate.
    {
      const { promise, resolve } = Promise.withResolvers();
      zlib.unzip(
        Buffer.concat([zlib.deflateSync('abc'), zlib.deflateSync('def')]),
        (err, result) => {
          assert.ifError(err);
          assert.strictEqual(result.toString(), abc);
          resolve();
        }
      );

      await promise;
    }

    {
      // Files that have the "right" magic bytes for starting a new gzip member
      // in the middle of themselves, even if they are part of a single
      // regularly compressed member
      const pmmDataZlib = Buffer.from(
        'eJztwTEBAAAAwqD1T+1vBqAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' +
          'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' +
          'AAAAAAAAAAAAAAAAAAAAAAAAAAAIAz+w4AAQ==',
        'base64'
      );
      const pmmDataGz = Buffer.from(
        'H4sIAMyK8lYCA+3BMQEAAADCoPVP7WENoAAAAAAAAAAAAAAAAAAAAAAAH4sAAAAAAAAAAAAAAAAA' +
          'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' +
          'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABuGn4Inv/6AQA=',
        'base64'
      );
      const pmmExpected = zlib.inflateSync(pmmDataZlib);
      const pmmResultBuffers = [];
      const { promise, resolve, reject } = Promise.withResolvers();

      Readable.from([pmmDataGz])
        .pipe(zlib.createGunzip())
        .on('error', reject)
        .on('data', (data) => pmmResultBuffers.push(data))
        .on('finish', resolve);

      await promise;
      // Result should match original random garbage
      assert.deepStrictEqual(Buffer.concat(pmmResultBuffers), pmmExpected);
    }

    // Test that the next gzip member can wrap around the input buffer boundary
    for (const offset of [0, 1, 2, 3, 4, defEncoded.length]) {
      const { promise, resolve, reject } = Promise.withResolvers();
      const resultBuffers = [];

      const unzip = zlib
        .createGunzip()
        .on('error', reject)
        .on('data', (data) => resultBuffers.push(data))
        .on('finish', resolve);

      // First write: write "abc" + the first bytes of "def"
      unzip.write(Buffer.concat([abcEncoded, defEncoded.slice(0, offset)]));

      // Write remaining bytes of "def"
      unzip.end(defEncoded.slice(offset));

      await promise;

      assert.strictEqual(
        Buffer.concat(resultBuffers).toString(),
        'abcdef',
        `result should match original input (offset = ${offset})`
      );
    }
  },
};

// Test taken from:
// https://github.com/nodejs/node/blob/fc02b88f89f8d5abf5ee4414a1026444c18d77b3/test/parallel/test-zlib-not-string-or-buffer.js
export const zlibNotStringOrBuffer = {
  test() {
    for (const input of [
      undefined,
      null,
      true,
      false,
      0,
      1,
      [1, 2, 3],
      { foo: 'bar' },
    ]) {
      assert.throws(
        () => zlib.deflateSync(input),
        // TODO(soon): Use same error code as NodeJS
        {
          name: 'TypeError',
        }
      );
    }
  },
};

// Test taken from:
// https://github.com/nodejs/node/blob/fc02b88f89f8d5abf5ee4414a1026444c18d77b3/test/parallel/test-zlib-from-gzip-with-trailing-garbage.js
export const zlibFromGzipWithTrailingGarbage = {
  async test() {
    // Should ignore trailing null-bytes
    let data = Buffer.concat([
      zlib.gzipSync('abc'),
      zlib.gzipSync('def'),
      Buffer.alloc(10),
    ]);

    assert.strictEqual(zlib.gunzipSync(data).toString(), 'abcdef');

    {
      const { promise, resolve } = Promise.withResolvers();

      zlib.gunzip(data, (err, result) => {
        assert.ifError(err);
        assert.strictEqual(
          result.toString(),
          'abcdef',
          `result '${result.toString()}' should match original string`
        );
        resolve();
      });

      await promise;
    }

    // If the trailing garbage happens to look like a gzip header, it should
    // throw an error.
    data = Buffer.concat([
      zlib.gzipSync('abc'),
      zlib.gzipSync('def'),
      Buffer.from([0x1f, 0x8b, 0xff, 0xff]),
      Buffer.alloc(10),
    ]);

    assert.throws(
      () => zlib.gunzipSync(data),
      /^Error: unknown compression method$/
    );

    {
      const { promise, resolve } = Promise.withResolvers();
      zlib.gunzip(data, (err, result) => {
        // TODO(soon): use same error code as NodeJS
        // TODO(soon): Do our messages have a redundant "Error: " word?
        assert.strictEqual(err.name, 'Error');
        assert.match(err.message, /unknown compression method/);
        assert.strictEqual(result, undefined);
        resolve();
      });

      await promise;
    }

    // In this case the trailing junk is too short to be a gzip segment
    // So we ignore it and decompression succeeds.
    data = Buffer.concat([
      zlib.gzipSync('abc'),
      zlib.gzipSync('def'),
      Buffer.from([0x1f, 0x8b, 0xff, 0xff]),
    ]);

    assert.throws(
      () => zlib.gunzipSync(data),
      /^Error: unknown compression method$/
    );

    {
      const { promise, resolve } = Promise.withResolvers();
      zlib.gunzip(data, (err, result) => {
        // TOOD(soon): Use same error code as NodeJS
        // TODO(soon): Do our messages have a redundant "Error: " word?
        assert.strictEqual(err.name, 'Error');
        assert.match(err.message, /unknown compression method/);
        assert.strictEqual(result, undefined);
        resolve();
      });

      await promise;
    }
  },
};

// Test taken from
// https://github.com/nodejs/node/blob/fc02b88f89f8d5abf5ee4414a1026444c18d77b3/test/parallel/test-zlib-from-string.js
export const zlibFromString = {
  async test() {
    const inputString =
      'ΩΩLorem ipsum dolor sit amet, consectetur adipiscing eli' +
      't. Morbi faucibus, purus at gravida dictum, libero arcu ' +
      'convallis lacus, in commodo libero metus eu nisi. Nullam' +
      ' commodo, neque nec porta placerat, nisi est fermentum a' +
      'ugue, vitae gravida tellus sapien sit amet tellus. Aenea' +
      'n non diam orci. Proin quis elit turpis. Suspendisse non' +
      ' diam ipsum. Suspendisse nec ullamcorper odio. Vestibulu' +
      'm arcu mi, sodales non suscipit id, ultrices ut massa. S' +
      'ed ac sem sit amet arcu malesuada fermentum. Nunc sed. ';
    const expectedBase64Deflate =
      'eJxdUUtOQzEMvMoc4OndgT0gJCT2buJWlpI4jePeqZfpmX' +
      'AKLRKbLOzx/HK73q6vOrhCunlF1qIDJhNUeW5I2ozT5OkD' +
      'lKWLJWkncJG5403HQXAkT3Jw29B9uIEmToMukglZ0vS6oc' +
      'iBh4JG8sV4oVLEUCitK2kxq1WzPnChHDzsaGKy491LofoA' +
      'bWh8do43oeuYhB5EPCjcLjzYJo48KrfQBvnJecNFJvHT1+' +
      'RSQsGoC7dn2t/xjhduTA1NWyQIZR0pbHwMDatnD+crPqKS' +
      'qGPHp1vnlsWM/07ubf7bheF7kqSj84Bm0R1fYTfaK8vqqq' +
      'fKBtNMhe3OZh6N95CTvMX5HJJi4xOVzCgUOIMSLH7wmeOH' +
      'aFE4RdpnGavKtrB5xzfO/Ll9';
    const expectedBase64Gzip =
      'H4sIAAAAAAAAA11RS05DMQy8yhzg6d2BPSAkJPZu4laWkjiN4' +
      '96pl+mZcAotEpss7PH8crverq86uEK6eUXWogMmE1R5bkjajN' +
      'Pk6QOUpYslaSdwkbnjTcdBcCRPcnDb0H24gSZOgy6SCVnS9Lq' +
      'hyIGHgkbyxXihUsRQKK0raTGrVbM+cKEcPOxoYrLj3Uuh+gBt' +
      'aHx2jjeh65iEHkQ8KNwuPNgmjjwqt9AG+cl5w0Um8dPX5FJCw' +
      'agLt2fa3/GOF25MDU1bJAhlHSlsfAwNq2cP5ys+opKoY8enW+' +
      'eWxYz/Tu5t/tuF4XuSpKPzgGbRHV9hN9ory+qqp8oG00yF7c5' +
      'mHo33kJO8xfkckmLjE5XMKBQ4gxIsfvCZ44doUThF2mcZq8q2' +
      'sHnHNzRtagj5AQAA';

    {
      const { promise, resolve } = Promise.withResolvers();
      zlib.deflate(inputString, (err, buffer) => {
        zlib.inflate(buffer, (err, inflated) => {
          assert.strictEqual(inflated.toString(), inputString);
          resolve();
        });
      });
      await promise;
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      zlib.gzip(inputString, (err, buffer) => {
        assert.ifError(err);
        // Can't actually guarantee that we'll get exactly the same
        // deflated bytes when we compress a string, since the header
        // depends on stuff other than the input string itself.
        // However, decrypting it should definitely yield the same
        // result that we're expecting, and this should match what we get
        // from inflating the known valid deflate data.
        zlib.gunzip(buffer, (err, gunzipped) => {
          assert.ifError(err);
          assert.strictEqual(gunzipped.toString(), inputString);
          resolve();
        });
      });
      await promise;
    }

    let buffer = Buffer.from(expectedBase64Deflate, 'base64');
    {
      const { promise, resolve } = Promise.withResolvers();
      zlib.unzip(buffer, (err, buffer) => {
        assert.ifError(err);
        assert.strictEqual(buffer.toString(), inputString);
        resolve();
      });

      await promise;
    }

    buffer = Buffer.from(expectedBase64Gzip, 'base64');

    {
      const { promise, resolve } = Promise.withResolvers();
      zlib.unzip(buffer, (err, buffer) => {
        assert.ifError(err);
        assert.strictEqual(buffer.toString(), inputString);
        resolve();
      });
      await promise;
    }
  },
};

// Test taken from
// https://github.com/nodejs/node/blob/fc02b88f89f8d5abf5ee4414a1026444c18d77b3/test/parallel/test-zlib-empty-buffer.js
export const zlibEmptyBuffer = {
  async test() {
    const emptyBuffer = Buffer.alloc(0);

    for (const [compress, decompress, method] of [
      [zlib.deflateRawSync, zlib.inflateRawSync, 'raw sync'],
      [zlib.deflateSync, zlib.inflateSync, 'deflate sync'],
      [zlib.gzipSync, zlib.gunzipSync, 'gzip sync'],
      [zlib.brotliCompressSync, zlib.brotliDecompressSync, 'br sync'],
      [promisify(zlib.deflateRaw), promisify(zlib.inflateRaw), 'raw'],
      [promisify(zlib.deflate), promisify(zlib.inflate), 'deflate'],
      [promisify(zlib.gzip), promisify(zlib.gunzip), 'gzip'],
      [promisify(zlib.brotliCompress), promisify(zlib.brotliDecompress), 'br'],
    ]) {
      const compressed = await compress(emptyBuffer);
      const decompressed = await decompress(compressed);
      assert.deepStrictEqual(
        emptyBuffer,
        decompressed,
        `Expected ${inspect(compressed)} to match ${inspect(decompressed)} ` +
          `to match for ${method}`
      );
    }
  },
};

// Test taken from:
// https://github.com/nodejs/node/blob/d75e253c506310ea6728329981beb3284fa431b5/test/parallel/test-zlib-flush.js
export const zlibFlush = {
  async test() {
    const opts = { level: 0 };
    const deflater = zlib.createDeflate(opts);

    const chunk = Buffer.from('/9j/4AAQSkZJRgABAQEASA==', 'base64');
    const expectedNone = Buffer.from([0x78, 0x01]);
    const blkhdr = Buffer.from([0x00, 0x10, 0x00, 0xef, 0xff]);
    const adler32 = Buffer.from([0x00, 0x00, 0x00, 0xff, 0xff]);
    const expectedFull = Buffer.concat([blkhdr, chunk, adler32]);
    let actualNone;
    let actualFull;

    const { promise, resolve } = Promise.withResolvers();
    deflater.write(chunk, function () {
      deflater.flush(zlib.constants.Z_NO_FLUSH, function () {
        actualNone = deflater.read();
        deflater.flush(function () {
          const bufs = [];
          let buf;
          while ((buf = deflater.read()) !== null) bufs.push(buf);
          actualFull = Buffer.concat(bufs);

          resolve();
        });
      });
    });

    await promise;
    assert.deepStrictEqual(actualNone, expectedNone);
    assert.deepStrictEqual(actualFull, expectedFull);
  },
};

// Test taken from:
// https://github.com/nodejs/node/blob/9bdf2ee1d184e7ec5c690319e068894ed324b595/test/parallel/test-zlib-dictionary.js
export const zlibDictionary = {
  async test() {
    const spdyDict = Buffer.from(
      [
        'optionsgetheadpostputdeletetraceacceptaccept-charsetaccept-encodingaccept-',
        'languageauthorizationexpectfromhostif-modified-sinceif-matchif-none-matchi',
        'f-rangeif-unmodifiedsincemax-forwardsproxy-authorizationrangerefererteuser',
        '-agent10010120020120220320420520630030130230330430530630740040140240340440',
        '5406407408409410411412413414415416417500501502503504505accept-rangesageeta',
        'glocationproxy-authenticatepublicretry-afterservervarywarningwww-authentic',
        'ateallowcontent-basecontent-encodingcache-controlconnectiondatetrailertran',
        'sfer-encodingupgradeviawarningcontent-languagecontent-lengthcontent-locati',
        'oncontent-md5content-rangecontent-typeetagexpireslast-modifiedset-cookieMo',
        'ndayTuesdayWednesdayThursdayFridaySaturdaySundayJanFebMarAprMayJunJulAugSe',
        'pOctNovDecchunkedtext/htmlimage/pngimage/jpgimage/gifapplication/xmlapplic',
        'ation/xhtmltext/plainpublicmax-agecharset=iso-8859-1utf-8gzipdeflateHTTP/1',
        '.1statusversionurl\0',
      ].join('')
    );

    const input = [
      'HTTP/1.1 200 Ok',
      'Server: node.js',
      'Content-Length: 0',
      '',
    ].join('\r\n');

    // basicDictionaryTest
    {
      let { promise, resolve, reject } = Promise.withResolvers();
      let output = '';
      const deflate = zlib.createDeflate({ dictionary: spdyDict });
      const inflate = zlib.createInflate({ dictionary: spdyDict });
      inflate.setEncoding('utf-8');

      deflate
        .on('data', function (chunk) {
          inflate.write(chunk);
        })
        .on('error', reject)
        .on('end', function () {
          inflate.end();
        });

      inflate
        .on('error', reject)
        .on('end', resolve)
        .on('data', function (chunk) {
          output += chunk;
        });

      deflate.write(input);
      deflate.end();
      await promise;
      assert.strictEqual(input, output);
    }

    // deflateResetDictionaryTest
    {
      let { promise, resolve, reject } = Promise.withResolvers();
      let doneReset = false;
      let output = '';
      const deflate = zlib.createDeflate({ dictionary: spdyDict });
      const inflate = zlib.createInflate({ dictionary: spdyDict });
      inflate.setEncoding('utf-8');

      deflate
        .on('data', function (chunk) {
          if (doneReset) inflate.write(chunk);
        })
        .on('error', reject)
        .on('end', function () {
          inflate.end();
        });

      inflate
        .on('data', function (chunk) {
          output += chunk;
        })
        .on('error', reject)
        .on('end', resolve);

      deflate.write(input);
      deflate.flush(function () {
        deflate.reset();
        doneReset = true;
        deflate.write(input);
        deflate.end();
      });

      await promise;
      assert.strictEqual(input, output);
    }

    // rawDictionaryTest
    {
      let { promise, resolve, reject } = Promise.withResolvers();
      let output = '';
      const deflate = zlib.createDeflateRaw({ dictionary: spdyDict });
      const inflate = zlib.createInflateRaw({ dictionary: spdyDict });
      inflate.setEncoding('utf-8');

      deflate
        .on('data', function (chunk) {
          inflate.write(chunk);
        })
        .on('error', reject)
        .on('end', function () {
          inflate.end();
        });
      inflate
        .on('data', function (chunk) {
          output += chunk;
        })
        .on('error', reject)
        .on('end', resolve);

      deflate.write(input);
      deflate.end();

      await promise;
      assert.strictEqual(input, output);
    }

    // deflateRawResetDictionaryTest
    {
      let { promise, resolve, reject } = Promise.withResolvers();
      let doneReset = false;
      let output = '';
      const deflate = zlib.createDeflateRaw({ dictionary: spdyDict });
      const inflate = zlib.createInflateRaw({ dictionary: spdyDict });
      inflate.setEncoding('utf-8');

      deflate
        .on('data', function (chunk) {
          if (doneReset) inflate.write(chunk);
        })
        .on('error', reject)
        .on('end', function () {
          inflate.end();
        });

      inflate
        .on('data', function (chunk) {
          output += chunk;
        })
        .on('error', reject)
        .on('end', resolve);

      deflate.write(input);
      deflate.flush(function () {
        deflate.reset();
        doneReset = true;
        deflate.write(input);
        deflate.end();
      });

      await promise;
      assert.strictEqual(input, output);
    }
  },
};

// Test taken from
// https://github.com/nodejs/node/blob/ef6b9ffc8dfc7b2a395c864d2729a0ce1be9ef18/test/parallel/test-zlib-close-after-write.js
export const closeAfterWrite = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    zlib.gzip('hello', (err, out) => {
      assert.ifError(err);

      const unzip = zlib.createGunzip();
      unzip.write(out);
      unzip.close(resolve);
    });
    await promise;
  },
};

export const inflateSyncTest = {
  test() {
    const BIG_DATA = 'horse'.repeat(50_000) + 'cow'.repeat(49_000);
    assert.strictEqual(
      zlib.inflateSync(zlib.deflateSync(BIG_DATA)).toString(),
      BIG_DATA
    );

    assert.throws(
      () => zlib.inflateSync('garbage data'),
      new Error('incorrect header check')
    );

    assert.strictEqual(
      zlib
        .inflateSync(Buffer.from('OE9LyixKUUiCEQAmfgUk', 'base64'), {
          windowBits: 11,
          level: 4,
        })
        .toString(),
      'bird bird bird'
    );
  },
};

export const zipBombTest = {
  test() {
    // 225 bytes (raw)
    const ZLIB_BOMB_3 =
      'eNqruPX2jqHeUkaJtov/25xEzoho9fL6aF70yPBZvUBsqZPmzHO+F253euVbWScs28QQ2LG' +
      'CpdBc8ef84mkfpubvs6n/Z/9vrk9b4oy4ffG2ujp8DAwMB074/p97traX8fCt43fPvbMDCh' +
      'mUtZ/2/rw58MHbhNLblkCBhC96ZuZ1Z+xY5vn3P5/LDBRxy3/6NTF7G+udupj/d4r5gSISe' +
      '6sPAikGD0EeINkgBuZkQDgSyJxRmVGZUZlRmVGZURlCMpZRcVE/bjGdi/fzr+O/n/P32XWD' +
      'nyxJaZvuAwDXRDs+';

    // 1799 bytes
    const zlib_bomb_2 = zlib.inflateSync(Buffer.from(ZLIB_BOMB_3, 'base64'));

    // ~ 1MB
    const zlib_bomb_1 = zlib.inflateSync(zlib_bomb_2);

    // Would be 1 GB, if we let it
    assert.throws(
      () => zlib.inflateSync(zlib_bomb_1),
      new RangeError('Memory limit exceeded')
    );
  },
};

export const deflateSyncTest = {
  test() {
    function maskOsId(buf) {
      // Clear the OS ID byte in gzip, which varies based on the platform used to run the tests
      return buf.fill(0x0, 9, 10);
    }

    assert.strictEqual(
      zlib
        .deflateSync('bird bird bird', { windowBits: 11, level: 4 })
        .toString('base64'),
      'OE9LyixKUUiCEQAmfgUk'
    );

    assert.deepStrictEqual(
      maskOsId(
        zlib.gzipSync('water, water, everywhere, nor any drop to drink')
      ).toString('base64'),
      'H4sIAAAAAAAAACtPLEkt0lEoh1CpZalFleUZqUWpOgp5+UUKiXmVCilF+QUKJfkKKUWZedkAqpLyPC8AAAA='
    );
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/5949e169bd886fcc965172f671671b47ac07d2e2/test/parallel/test-zlib-premature-end.js
export const prematureEnd = {
  async test() {
    const input = '0123456789'.repeat(4);

    for (const [compress, decompressor] of [
      [zlib.deflateRawSync, zlib.createInflateRaw],
      [zlib.deflateSync, zlib.createInflate],
      [zlib.brotliCompressSync, zlib.createBrotliDecompress],
    ]) {
      const compressed = compress(input);
      const trailingData = Buffer.from('not valid compressed data');

      for (const variant of [
        (stream) => {
          stream.end(compressed);
        },
        (stream) => {
          stream.write(compressed);
          stream.write(trailingData);
        },
        (stream) => {
          stream.write(compressed);
          stream.end(trailingData);
        },
        (stream) => {
          stream.write(Buffer.concat([compressed, trailingData]));
        },
        (stream) => {
          stream.end(Buffer.concat([compressed, trailingData]));
        },
      ]) {
        let output = '';
        const { promise, resolve, reject } = Promise.withResolvers();
        const stream = decompressor();
        stream.setEncoding('utf8');
        stream
          .on('data', (chunk) => (output += chunk))
          .on('end', () => {
            assert.strictEqual(output, input);
            assert.strictEqual(stream.bytesWritten, compressed.length);
            resolve();
          })
          .on('error', reject);
        variant(stream);
        await promise;
      }
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/5949e169bd886fcc965172f671671b47ac07d2e2/test/parallel/test-zlib-write-after-flush.js
export const writeAfterFlush = {
  async test() {
    for (const [createCompress, createDecompress] of [
      [zlib.createGzip, zlib.createGunzip],
      [zlib.createBrotliCompress, zlib.createBrotliDecompress],
    ]) {
      const { promise, resolve, reject } = Promise.withResolvers();
      const gzip = createCompress();
      const gunz = createDecompress();

      gzip.pipe(gunz);

      let output = '';
      const input = 'A line of data\n';
      gunz.setEncoding('utf8');
      gunz
        .on('error', reject)
        .on('data', (c) => (output += c))
        .on('end', resolve);

      // Make sure that flush/write doesn't trigger an assert failure
      gzip.flush();
      gzip.write(input);
      gzip.end();
      gunz.read(0);
      await promise;
      assert.strictEqual(output, input);
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/5949e169bd886fcc965172f671671b47ac07d2e2/test/parallel/test-zlib-destroy-pipe.js
export const destroyPipe = {
  async test() {
    const ts = zlib.createGzip();
    const { promise, resolve } = Promise.withResolvers();

    const ws = new Writable({
      write: (chunk, enc, cb) => {
        queueMicrotask(cb);
        ts.destroy();
        resolve();
      },
    });

    const buf = Buffer.allocUnsafe(1024 * 1024 * 20);
    ts.end(buf);
    ts.pipe(ws);
    await promise;
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/5949e169bd886fcc965172f671671b47ac07d2e2/test/parallel/test-zlib-write-after-end.js
export const writeAfterEnd = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const data = zlib.deflateRawSync('Welcome');
    const inflate = zlib.createInflateRaw();
    const writeCallback = mock.fn();
    inflate.resume();
    inflate.write(data, writeCallback);
    inflate.write(Buffer.from([0x00]), writeCallback);
    inflate.write(Buffer.from([0x00]), writeCallback);
    inflate.flush(resolve);
    await promise;
    assert.strictEqual(writeCallback.mock.callCount(), 3);
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/5949e169bd886fcc965172f671671b47ac07d2e2/test/parallel/test-zlib-convenience-methods.js
export const convenienceMethods = {
  async test() {
    // Must be a multiple of 4 characters in total to test all ArrayBufferView
    // types.
    const expectStr = 'blah'.repeat(8);
    const expectBuf = Buffer.from(expectStr);

    const opts = {
      level: 9,
      chunkSize: 1024,
    };

    const optsInfo = {
      info: true,
    };

    for (const [type, expect] of [
      ['string', expectStr],
      ['Buffer', expectBuf],
    ]) {
      for (const method of [
        ['gzip', 'gunzip', 'Gzip', 'Gunzip'],
        ['gzip', 'unzip', 'Gzip', 'Unzip'],
        ['deflate', 'inflate', 'Deflate', 'Inflate'],
        ['deflateRaw', 'inflateRaw', 'DeflateRaw', 'InflateRaw'],
        [
          'brotliCompress',
          'brotliDecompress',
          'BrotliCompress',
          'BrotliDecompress',
        ],
      ]) {
        {
          const { promise, resolve } = Promise.withResolvers();
          zlib[method[0]](expect, opts, (err, result) => {
            assert.ifError(err);
            zlib[method[1]](result, opts, (err, result) => {
              assert.ifError(err);
              assert.strictEqual(
                result.toString(),
                expectStr,
                `Should get original string after ${method[0]}/` +
                  `${method[1]} ${type} with options.`
              );
              resolve();
            });
          });
          await promise;
        }

        {
          const { promise, resolve } = Promise.withResolvers();
          zlib[method[0]](expect, (err, result) => {
            assert.ifError(err);
            zlib[method[1]](result, (err, result) => {
              assert.ifError(err);
              assert.strictEqual(
                result.toString(),
                expectStr,
                `Should get original string after ${method[0]}/` +
                  `${method[1]} ${type} without options.`
              );
              resolve();
            });
          });
          await promise;
        }

        // TODO(soon): Enable this test
        // {
        //   const { promise, resolve } = Promise.withResolvers();
        //   zlib[method[0]](expect, optsInfo, (err, result) => {
        //     assert.ifError(err);
        //
        //     const compressed = result.buffer;
        //     zlib[method[1]](compressed, optsInfo, (err, result) => {
        //       assert.ifError(err);
        //       assert.strictEqual(
        //         result.buffer.toString(),
        //         expectStr,
        //         `Should get original string after ${method[0]}/` +
        //           `${method[1]} ${type} with info option.`
        //       );
        //       resolve();
        //     });
        //   });
        //   await promise;
        // }

        {
          const compressed = zlib[`${method[0]}Sync`](expect, opts);
          const decompressed = zlib[`${method[1]}Sync`](compressed, opts);
          assert.strictEqual(
            decompressed.toString(),
            expectStr,
            `Should get original string after ${method[0]}Sync/` +
              `${method[1]}Sync ${type} with options.`
          );
        }

        {
          const compressed = zlib[`${method[0]}Sync`](expect);
          const decompressed = zlib[`${method[1]}Sync`](compressed);
          assert.strictEqual(
            decompressed.toString(),
            expectStr,
            `Should get original string after ${method[0]}Sync/` +
              `${method[1]}Sync ${type} without options.`
          );
        }

        // TODO(soon): Enable this test
        // {
        //   const compressed = zlib[`${method[0]}Sync`](expect, optsInfo);
        //   const decompressed = zlib[`${method[1]}Sync`](
        //     compressed.buffer,
        //     optsInfo
        //   );
        //   assert.strictEqual(
        //     decompressed.buffer.toString(),
        //     expectStr,
        //     `Should get original string after ${method[0]}Sync/` +
        //       `${method[1]}Sync ${type} without options.`
        //   );
        //   assert.ok(
        //     decompressed.engine instanceof zlib[method[3]],
        //     `Should get engine ${method[3]} after ${method[0]} ` +
        //       `${type} with info option.`
        //   );
        // }
      }
    }

    assert.throws(() => zlib.gzip('abc'), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
      message:
        'The "callback" argument must be of type function. ' +
        'Received undefined',
    });
  },
};

// Test taken from
// https://github.com/nodejs/node/blob/2bd6a57b7b934afcaf437a90e2abebcb79c13acf/test/parallel/test-zlib-brotli-from-string.js
export const brotliFromString = {
  async test() {
    const inputString =
      'ΩΩLorem ipsum dolor sit amet, consectetur adipiscing eli' +
      't. Morbi faucibus, purus at gravida dictum, libero arcu ' +
      'convallis lacus, in commodo libero metus eu nisi. Nullam' +
      ' commodo, neque nec porta placerat, nisi est fermentum a' +
      'ugue, vitae gravida tellus sapien sit amet tellus. Aenea' +
      'n non diam orci. Proin quis elit turpis. Suspendisse non' +
      ' diam ipsum. Suspendisse nec ullamcorper odio. Vestibulu' +
      'm arcu mi, sodales non suscipit id, ultrices ut massa. S' +
      'ed ac sem sit amet arcu malesuada fermentum. Nunc sed. ';
    const compressedString =
      'G/gBQBwHdky2aHV5KK9Snf05//1pPdmNw/7232fnIm1IB' +
      'K1AA8RsN8OB8Nb7Lpgk3UWWUlzQXZyHQeBBbXMTQXC1j7' +
      'wg3LJs9LqOGHRH2bj/a2iCTLLx8hBOyTqgoVuD1e+Qqdn' +
      'f1rkUNyrWq6LtOhWgxP3QUwdhKGdZm3rJWaDDBV7+pDk1' +
      'MIkrmjp4ma2xVi5MsgJScA3tP1I7mXeby6MELozrwoBQD' +
      'mVTnEAicZNj4lkGqntJe2qSnGyeMmcFgraK94vCg/4iLu' +
      'Tw5RhKhnVY++dZ6niUBmRqIutsjf5TzwF5iAg8a9UkjF5' +
      '2eZ0tB2vo6v8SqVfNMkBmmhxr0NT9LkYF69aEjlYzj7IE' +
      'KmEUQf1HBogRYhFIt4ymRNEgHAIzOyNEsQM=';

    {
      const { promise, resolve } = Promise.withResolvers();

      zlib.brotliCompress(inputString, (err, buffer) => {
        assert.ifError(err);
        assert(inputString.length > buffer.length);

        zlib.brotliDecompress(buffer, (err, buffer) => {
          assert.ifError(err);
          assert.strictEqual(buffer.toString(), inputString);
          resolve();
        });
      });
      await promise;
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      const buffer = Buffer.from(compressedString, 'base64');
      zlib.brotliDecompress(buffer, (err, buffer) => {
        assert.ifError(err);
        assert.strictEqual(buffer.toString(), inputString);
        resolve();
      });

      await promise;
    }
  },
};

// Test taken from
// https://github.com/nodejs/node/blob/26eb062a9b9c0ae8cee9cb5c378e43bca363207c/test/parallel/test-zlib-maxOutputLength.js
export const maxOutputLength = {
  async test() {
    const encoded = Buffer.from('G38A+CXCIrFAIAM=', 'base64');

    // Async
    {
      const { promise, resolve } = Promise.withResolvers();
      zlib.brotliDecompress(encoded, { maxOutputLength: 64 }, (err) => {
        // TODO(soon): Make error the same as NodeJS
        assert.match(err.message, /Memory limit exceeded/);
        resolve();
      });
      await promise;
    }

    // Sync
    assert.throws(function () {
      zlib.brotliDecompressSync(encoded, { maxOutputLength: 64 });
    }, RangeError);

    // Async
    {
      const { promise, resolve } = Promise.withResolvers();
      zlib.brotliDecompress(encoded, { maxOutputLength: 256 }, function (err) {
        assert.strictEqual(err, null);
        resolve();
      });

      await promise;
    }

    // Sync
    zlib.brotliDecompressSync(encoded, { maxOutputLength: 256 });
  },
};

// Test taken from
// https://github.com/nodejs/node/blob/24302c9fe94e1dd755ac8a8cc1f6aa4444f75cb3/test/parallel/test-zlib-invalid-arg-value-brotli-compress.js
export const invalidArgValueBrotliCompress = {
  test() {
    const opts = {
      params: {
        [zlib.constants.BROTLI_PARAM_MODE]: 'lol',
      },
    };

    // TODO(soon): Node's test invokes BrotliCompress without new, but we barf if you try that.
    assert.throws(() => new zlib.BrotliCompress(opts), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
  },
};

// Test taken from
// https://github.com/nodejs/node/blob/24302c9fe94e1dd755ac8a8cc1f6aa4444f75cb3/test/parallel/test-zlib-brotli-flush.js
export const brotliFlush = {
  async test() {
    const deflater = new zlib.BrotliCompress();

    const chunk = Buffer.from('/9j/4AAQSkZJRgABAQEASA==', 'base64');
    const expectedFull = Buffer.from('iweA/9j/4AAQSkZJRgABAQEASA==', 'base64');
    let actualFull;

    {
      const { promise, resolve } = Promise.withResolvers();
      deflater.write(chunk, function () {
        deflater.flush(function () {
          const bufs = [];
          let buf;
          while ((buf = deflater.read()) !== null) bufs.push(buf);
          actualFull = Buffer.concat(bufs);
          resolve();
        });
      });

      await promise;
    }
    assert.deepStrictEqual(actualFull, expectedFull);
  },
};

// Test taken from
// https://github.com/nodejs/node/blob/24302c9fe94e1dd755ac8a8cc1f6aa4444f75cb3/test/parallel/test-zlib-brotli.js
export const brotli = {
  async test() {
    {
      const sampleBuffer = Buffer.from(PSS_VECTORS_JSON);

      // Test setting the quality parameter at stream creation:
      const sizes = [];
      for (
        let quality = zlib.constants.BROTLI_MIN_QUALITY;
        quality <= zlib.constants.BROTLI_MAX_QUALITY;
        quality++
      ) {
        const encoded = zlib.brotliCompressSync(sampleBuffer, {
          params: {
            [zlib.constants.BROTLI_PARAM_QUALITY]: quality,
          },
        });
        sizes.push(encoded.length);
      }

      // Increasing quality should roughly correspond to decreasing compressed size:
      for (let i = 0; i < sizes.length - 1; i++) {
        assert(sizes[i + 1] <= sizes[i] * 1.05, sizes); // 5 % margin of error.
      }
      assert(sizes[0] > sizes[sizes.length - 1], sizes);
    }

    {
      // Test that setting out-of-bounds option values or keys fails.
      assert.throws(
        () => {
          zlib.createBrotliCompress({
            params: {
              10000: 0,
            },
          });
        },
        {
          code: 'ERR_BROTLI_INVALID_PARAM',
          name: 'RangeError',
          message: '10000 is not a valid Brotli parameter',
        }
      );

      // Test that accidentally using duplicate keys fails.
      assert.throws(
        () => {
          zlib.createBrotliCompress({
            params: {
              0: 0,
              '00': 0,
            },
          });
        },
        {
          code: 'ERR_BROTLI_INVALID_PARAM',
          name: 'RangeError',
          message: '00 is not a valid Brotli parameter',
        }
      );

      {
        assert.throws(
          () => {
            zlib.createBrotliCompress({
              params: {
                // This is a boolean flag
                [zlib.constants.BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING]:
                  42,
              },
            });
          },
          {
            code: 'ERR_ZLIB_INITIALIZATION_FAILED',
            name: 'Error',
            message: 'Initialization failed',
          }
        );
      }

      {
        // Test options.flush range
        // TODO(soon): Use the same code and message as NodeJS
        assert.throws(
          () => {
            zlib.brotliCompressSync('', { flush: zlib.constants.Z_FINISH });
          },
          {
            //code: 'ERR_OUT_OF_RANGE',
            name: 'RangeError',
            message:
              'The value of "options.flush" is out of range. It must be >= 0 ' +
              'and <= 3. Received 4',
          }
        );

        assert.throws(
          () => {
            zlib.brotliCompressSync('', {
              finishFlush: zlib.constants.Z_FINISH,
            });
          },
          {
            //code: 'ERR_OUT_OF_RANGE',
            name: 'RangeError',
            message:
              'The value of "options.finishFlush" is out of range. It must be ' +
              '>= 0 and <= 3. Received 4',
          }
        );
      }
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/2ef4b15604082abfd7aa26a4619a46802258ff3c/test/parallel/test-zlib-random-byte-pipes.js
export const zlibRandomBytePipes = {
  async test() {
    // Emit random bytes, and keep a shasum
    class RandomReadStream extends Stream {
      constructor(opt) {
        super();

        this.readable = true;
        this._paused = false;
        this._processing = false;
        this._hasher = crypto.createHash('sha1');
        opt ??= {};

        // base block size.
        opt.block ??= 256 * 1024;
        // Total number of bytes to emit
        opt.total ??= 256 * 1024 * 1024;
        this._remaining = opt.total;
        // How variable to make the block sizes
        opt.jitter ??= 1024;
        this._opt = opt;
        this._process = this._process.bind(this);
        queueMicrotask(this._process);
      }

      pause() {
        this._paused = true;
        this.emit('pause');
      }

      resume() {
        this._paused = false;
        this.emit('resume');
        this._process();
      }

      _process() {
        if (this._processing) return;
        if (this._paused) return;
        this._processing = true;

        if (!this._remaining) {
          this._hash = this._hasher.digest('hex').toLowerCase().trim();
          this._processing = false;

          this.emit('end');
          return;
        }

        // Figure out how many bytes to output
        // if finished, then just emit end.
        let block = this._opt.block;
        const jitter = this._opt.jitter;
        if (jitter) {
          block += Math.ceil(Math.random() * jitter - jitter / 2);
        }
        block = Math.min(block, this._remaining);
        const buf = Buffer.allocUnsafe(block);
        for (let i = 0; i < block; i++) {
          buf[i] = Math.random() * 256;
        }

        this._hasher.update(buf);
        this._remaining -= block;
        this._processing = false;
        this.emit('data', buf);
        queueMicrotask(this._process);
      }
    }

    // A filter that just verifies a shasum
    class HashStream extends Stream {
      constructor() {
        super();
        this.readable = this.writable = true;
        this._hasher = crypto.createHash('sha1');
      }

      write(c) {
        // Simulate the way that an fs.ReadStream returns false
        // on *every* write, only to resume a moment later.
        this._hasher.update(c);
        queueMicrotask(() => this.resume());
        return false;
      }

      resume() {
        this.emit('resume');
        queueMicrotask(() => this.emit('drain'));
      }

      end(c) {
        if (c) {
          this.write(c);
        }
        this._hash = this._hasher.digest('hex').toLowerCase().trim();
        this.emit('data', this._hash);
        this.emit('end');
      }
    }

    for (const [createCompress, createDecompress] of [
      [zlib.createGzip, zlib.createGunzip],
      [zlib.createBrotliCompress, zlib.createBrotliDecompress],
    ]) {
      const { promise, resolve, reject } = Promise.withResolvers();
      const inp = new RandomReadStream({ total: 1024, block: 256, jitter: 16 });
      const out = new HashStream();
      const gzip = createCompress();
      const gunz = createDecompress();

      inp.pipe(gzip).pipe(gunz).pipe(out);

      const onDataFn = mock.fn();
      onDataFn.mock.mockImplementation((c) => {
        assert.strictEqual(c, inp._hash, `Hash '${c}' equals '${inp._hash}'.`);
      });

      out.on('data', onDataFn).on('end', resolve).on('error', reject);

      await promise;

      assert.ok(onDataFn.mock.callCount() > 0, 'Should have called onData');
    }
  },
};

// Node.js tests relevant to zlib
//
// - [ ] test-zlib-brotli-16GB.js
// - [x] test-zlib-convenience-methods.js
// - [ ] test-zlib-flush-drain.js
// - [ ] test-zlib-invalid-input-memory.js
// - [ ] test-zlib-sync-no-event.js
// - [x] test-zlib-brotli-flush.js
// - [x] test-zlib-crc32.js
// - [ ] test-zlib-flush-drain-longblock.js
// - [ ] test-zlib.js
// - [x] test-zlib-truncated.js
// - [ ] test-zlib-brotli-from-brotli.js
// - [x] test-zlib-create-raw.js
// - [x] test-zlib-flush-flags.js
// - [ ] test-zlib-kmaxlength-rangeerror.js
// - [ ] test-zlib-unused-weak.js
// - [x] test-zlib-brotli-from-string.js
// - [x] test-zlib-deflate-constructors.js
// - [x] test-zlib-flush.js
// - [x] test-zlib-maxOutputLength.js
// - [x] test-zlib-unzip-one-byte-chunks.js
// - [x] test-zlib-brotli.js
// - [ ] test-zlib-deflate-raw-inherits.js
// - [ ] test-zlib-flush-write-sync-interleaved.js
// - [N/A] test-zlib-no-stream.js
// - [x] test-zlib-write-after-close.js
// - [ ] test-zlib-brotli-kmaxlength-rangeerror.js
// - [x] test-zlib-destroy.js
// - [x] test-zlib-from-concatenated-gzip.js
// - [x] test-zlib-not-string-or-buffer.js
// - [x] test-zlib-write-after-end.js
// - [x] test-zlib-bytes-read.js
// - [x] test-zlib-destroy-pipe.js
// - [ ] test-zlib-from-gzip.js
// - [x] test-zlib-object-write.js
// - [x] test-zlib-write-after-flush.js
// - [x] test-zlib-close-after-error.js
// - [x] test-zlib-dictionary-fail.js
// - [x] test-zlib-from-gzip-with-trailing-garbage.js
// - [ ] test-zlib-params.js
// - [x] test-zlib-zero-byte.js
// - [x] test-zlib-close-after-write.js
// - [x] test-zlib-dictionary.js
// - [x] test-zlib-from-string.js
// - [x] test-zlib-premature-end.js
// - [x] test-zlib-zero-windowBits.js
// - [x] test-zlib-close-in-ondata.js
// - [x] test-zlib-empty-buffer.js
// - [x] test-zlib-invalid-arg-value-brotli-compress.js
// - [x] test-zlib-random-byte-pipes.js
// - [x] test-zlib-const.js
// - [x] test-zlib-failed-init.js
// - [x] test-zlib-invalid-input.js
// - [x] test-zlib-reset-before-write.js

// Large test data is added at the end of the file in order to make the test code itself more readable
const PSS_VECTORS_JSON = `{
  "example01": {
    "publicKey": [
      "-----BEGIN PUBLIC KEY-----",
      "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQClbkoOcBAXWJpRh9x+qEHRVvLs",
      "DjatUqRN/rHmH3rZkdjFEFb/7bFitMDyg6EqiKOU3/Umq3KRy7MHzqv84LHf1c2V",
      "CAltWyuLbfXWce9jd8CSHLI8Jwpw4lmOb/idGfEFrMLT8Ms18pKA4Thrb2TE7yLh",
      "4fINDOjP+yJJvZohNwIDAQAB",
      "-----END PUBLIC KEY-----"
    ],
    "tests": [
      {
        "message": "cdc87da223d786df3b45e0bbbc721326d1ee2af806cc315475cc6f0d9c66e1b62371d45ce2392e1ac92844c310102f156a0d8d52c1f4c40ba3aa65095786cb769757a6563ba958fed0bcc984e8b517a3d5f515b23b8a41e74aa867693f90dfb061a6e86dfaaee64472c00e5f20945729cbebe77f06ce78e08f4098fba41f9d6193c0317e8b60d4b6084acb42d29e3808a3bc372d85e331170fcbf7cc72d0b71c296648b3a4d10f416295d0807aa625cab2744fd9ea8fd223c42537029828bd16be02546f130fd2e33b936d2676e08aed1b73318b750a0167d0",
        "salt": "dee959c7e06411361420ff80185ed57f3e6776af",
        "signature": "9074308fb598e9701b2294388e52f971faac2b60a5145af185df5287b5ed2887e57ce7fd44dc8634e407c8e0e4360bc226f3ec227f9d9e54638e8d31f5051215df6ebb9c2f9579aa77598a38f914b5b9c1bd83c4e2f9f382a0d0aa3542ffee65984a601bc69eb28deb27dca12c82c2d4c3f66cd500f1ff2b994d8a4e30cbb33c"
      },
      {
        "message": "851384cdfe819c22ed6c4ccb30daeb5cf059bc8e1166b7e3530c4c233e2b5f8f71a1cca582d43ecc72b1bca16dfc7013226b9e",
        "salt": "ef2869fa40c346cb183dab3d7bffc98fd56df42d",
        "signature": "3ef7f46e831bf92b32274142a585ffcefbdca7b32ae90d10fb0f0c729984f04ef29a9df0780775ce43739b97838390db0a5505e63de927028d9d29b219ca2c4517832558a55d694a6d25b9dab66003c4cccd907802193be5170d26147d37b93590241be51c25055f47ef62752cfbe21418fafe98c22c4d4d47724fdb5669e843"
      },
      {
        "message": "a4b159941761c40c6a82f2b80d1b94f5aa2654fd17e12d588864679b54cd04ef8bd03012be8dc37f4b83af7963faff0dfa225477437c48017ff2be8191cf3955fc07356eab3f322f7f620e21d254e5db4324279fe067e0910e2e81ca2cab31c745e67a54058eb50d993cdb9ed0b4d029c06d21a94ca661c3ce27fae1d6cb20f4564d66ce4767583d0e5f060215b59017be85ea848939127bd8c9c4d47b51056c031cf336f17c9980f3b8f5b9b6878e8b797aa43b882684333e17893fe9caa6aa299f7ed1a18ee2c54864b7b2b99b72618fb02574d139ef50f019c9eef416971338e7d470",
        "salt": "710b9c4747d800d4de87f12afdce6df18107cc77",
        "signature": "666026fba71bd3e7cf13157cc2c51a8e4aa684af9778f91849f34335d141c00154c4197621f9624a675b5abc22ee7d5baaffaae1c9baca2cc373b3f33e78e6143c395a91aa7faca664eb733afd14d8827259d99a7550faca501ef2b04e33c23aa51f4b9e8282efdb728cc0ab09405a91607c6369961bc8270d2d4f39fce612b1"
      },
      {
        "message": "bc656747fa9eafb3f0",
        "salt": "056f00985de14d8ef5cea9e82f8c27bef720335e",
        "signature": "4609793b23e9d09362dc21bb47da0b4f3a7622649a47d464019b9aeafe53359c178c91cd58ba6bcb78be0346a7bc637f4b873d4bab38ee661f199634c547a1ad8442e03da015b136e543f7ab07c0c13e4225b8de8cce25d4f6eb8400f81f7e1833b7ee6e334d370964ca79fdb872b4d75223b5eeb08101591fb532d155a6de87"
      },
      {
        "message": "b45581547e5427770c768e8b82b75564e0ea4e9c32594d6bff706544de0a8776c7a80b4576550eee1b2acabc7e8b7d3ef7bb5b03e462c11047eadd00629ae575480ac1470fe046f13a2bf5af17921dc4b0aa8b02bee6334911651d7f8525d10f32b51d33be520d3ddf5a709955a3dfe78283b9e0ab54046d150c177f037fdccc5be4ea5f68b5e5a38c9d7edcccc4975f455a6909b4",
        "salt": "80e70ff86a08de3ec60972b39b4fbfdcea67ae8e",
        "signature": "1d2aad221ca4d31ddf13509239019398e3d14b32dc34dc5af4aeaea3c095af73479cf0a45e5629635a53a018377615b16cb9b13b3e09d671eb71e387b8545c5960da5a64776e768e82b2c93583bf104c3fdb23512b7b4e89f633dd0063a530db4524b01c3f384c09310e315a79dcd3d684022a7f31c865a664e316978b759fad"
      },
      {
        "message": "10aae9a0ab0b595d0841207b700d48d75faedde3b775cd6b4cc88ae06e4694ec74ba18f8520d4f5ea69cbbe7cc2beba43efdc10215ac4eb32dc302a1f53dc6c4352267e7936cfebf7c8d67035784a3909fa859c7b7b59b8e39c5c2349f1886b705a30267d402f7486ab4f58cad5d69adb17ab8cd0ce1caf5025af4ae24b1fb8794c6070cc09a51e2f9911311e3877d0044c71c57a993395008806b723ac38373d395481818528c1e7053739282053529510e935cd0fa77b8fa53cc2d474bd4fb3cc5c672d6ffdc90a00f9848712c4bcfe46c60573659b11e6457e861f0f604b6138d144f8ce4e2da73",
        "salt": "a8ab69dd801f0074c2a1fc60649836c616d99681",
        "signature": "2a34f6125e1f6b0bf971e84fbd41c632be8f2c2ace7de8b6926e31ff93e9af987fbc06e51e9be14f5198f91f3f953bd67da60a9df59764c3dc0fe08e1cbef0b75f868d10ad3fba749fef59fb6dac46a0d6e504369331586f58e4628f39aa278982543bc0eeb537dc61958019b394fb273f215858a0a01ac4d650b955c67f4c58"
      }
    ]
  },
  "example10": {
    "publicKey": [
      "-----BEGIN PUBLIC KEY-----",
      "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEApd2GesTLAvkLlFfUjBSn",
      "cO+ZHFbDnA7GX9Ea+ok3zqV7m+esc7RcABdhW4LWIuMYdTtgJ8D9FXvhL4CQ/uKn",
      "rc0O73WfiLpJl8ekLVjJqhLLma4AH+UhwTu1QxRFqNWuT15MfpSKwifTYEBx8g5X",
      "fpBfvrFd+vBtHeWuYlPWOmohILMaXaXavJVQYA4g8n03OeJieSX+o8xQnyHf8E5u",
      "6kVJxUDWgJ/5MH7t6R//WHM9g4WiN9bTcFoz45GQCZIHDfet8TV89+NwDONmfeg/",
      "F7jfF3jbOB3OCctK0FilEQAac4GY7ifPVaE7dUU5kGWC7IsXS9WNXR89dnxhNyGu",
      "BQIDAQAB",
      "-----END PUBLIC KEY-----"
    ],
    "tests": [
      {
        "message": "883177e5126b9be2d9a9680327d5370c6f26861f5820c43da67a3ad609",
        "salt": "04e215ee6ff934b9da70d7730c8734abfcecde89",
        "signature": "82c2b160093b8aa3c0f7522b19f87354066c77847abf2a9fce542d0e84e920c5afb49ffdfdace16560ee94a1369601148ebad7a0e151cf16331791a5727d05f21e74e7eb811440206935d744765a15e79f015cb66c532c87a6a05961c8bfad741a9a6657022894393e7223739796c02a77455d0f555b0ec01ddf259b6207fd0fd57614cef1a5573baaff4ec00069951659b85f24300a25160ca8522dc6e6727e57d019d7e63629b8fe5e89e25cc15beb3a647577559299280b9b28f79b0409000be25bbd96408ba3b43cc486184dd1c8e62553fa1af4040f60663de7f5e49c04388e257f1ce89c95dab48a315d9b66b1b7628233876ff2385230d070d07e1666"
      },
      {
        "message": "dd670a01465868adc93f26131957a50c52fb777cdbaa30892c9e12361164ec13979d43048118e4445db87bee58dd987b3425d02071d8dbae80708b039dbb64dbd1de5657d9fed0c118a54143742e0ff3c87f74e45857647af3f79eb0a14c9d75ea9a1a04b7cf478a897a708fd988f48e801edb0b7039df8c23bb3c56f4e821ac",
        "salt": "8b2bdd4b40faf545c778ddf9bc1a49cb57f9b71b",
        "signature": "14ae35d9dd06ba92f7f3b897978aed7cd4bf5ff0b585a40bd46ce1b42cd2703053bb9044d64e813d8f96db2dd7007d10118f6f8f8496097ad75e1ff692341b2892ad55a633a1c55e7f0a0ad59a0e203a5b8278aec54dd8622e2831d87174f8caff43ee6c46445345d84a59659bfb92ecd4c818668695f34706f66828a89959637f2bf3e3251c24bdba4d4b7649da0022218b119c84e79a6527ec5b8a5f861c159952e23ec05e1e717346faefe8b1686825bd2b262fb2531066c0de09acde2e4231690728b5d85e115a2f6b92b79c25abc9bd9399ff8bcf825a52ea1f56ea76dd26f43baafa18bfa92a504cbd35699e26d1dcc5a2887385f3c63232f06f3244c3"
      },
      {
        "message": "48b2b6a57a63c84cea859d65c668284b08d96bdcaabe252db0e4a96cb1bac6019341db6fbefb8d106b0e90eda6bcc6c6262f37e7ea9c7e5d226bd7df85ec5e71efff2f54c5db577ff729ff91b842491de2741d0c631607df586b905b23b91af13da12304bf83eca8a73e871ff9db",
        "salt": "4e96fc1b398f92b44671010c0dc3efd6e20c2d73",
        "signature": "6e3e4d7b6b15d2fb46013b8900aa5bbb3939cf2c095717987042026ee62c74c54cffd5d7d57efbbf950a0f5c574fa09d3fc1c9f513b05b4ff50dd8df7edfa20102854c35e592180119a70ce5b085182aa02d9ea2aa90d1df03f2daae885ba2f5d05afdac97476f06b93b5bc94a1a80aa9116c4d615f333b098892b25fface266f5db5a5a3bcc10a824ed55aad35b727834fb8c07da28fcf416a5d9b2224f1f8b442b36f91e456fdea2d7cfe3367268de0307a4c74e924159ed33393d5e0655531c77327b89821bdedf880161c78cd4196b5419f7acc3f13e5ebf161b6e7c6724716ca33b85c2e25640192ac2859651d50bde7eb976e51cec828b98b6563b86bb"
      },
      {
        "message": "0b8777c7f839baf0a64bbbdbc5ce79755c57a205b845c174e2d2e90546a089c4e6ec8adffa23a7ea97bae6b65d782b82db5d2b5a56d22a29a05e7c4433e2b82a621abba90add05ce393fc48a840542451a",
        "salt": "c7cd698d84b65128d8835e3a8b1eb0e01cb541ec",
        "signature": "34047ff96c4dc0dc90b2d4ff59a1a361a4754b255d2ee0af7d8bf87c9bc9e7ddeede33934c63ca1c0e3d262cb145ef932a1f2c0a997aa6a34f8eaee7477d82ccf09095a6b8acad38d4eec9fb7eab7ad02da1d11d8e54c1825e55bf58c2a23234b902be124f9e9038a8f68fa45dab72f66e0945bf1d8bacc9044c6f07098c9fcec58a3aab100c805178155f030a124c450e5acbda47d0e4f10b80a23f803e774d023b0015c20b9f9bbe7c91296338d5ecb471cafb032007b67a60be5f69504a9f01abb3cb467b260e2bce860be8d95bf92c0c8e1496ed1e528593a4abb6df462dde8a0968dffe4683116857a232f5ebf6c85be238745ad0f38f767a5fdbf486fb"
      },
      {
        "message": "f1036e008e71e964dadc9219ed30e17f06b4b68a955c16b312b1eddf028b74976bed6b3f6a63d4e77859243c9cccdc98016523abb02483b35591c33aad81213bb7c7bb1a470aabc10d44256c4d4559d916",
        "salt": "efa8bff96212b2f4a3f371a10d574152655f5dfb",
        "signature": "7e0935ea18f4d6c1d17ce82eb2b3836c55b384589ce19dfe743363ac9948d1f346b7bfddfe92efd78adb21faefc89ade42b10f374003fe122e67429a1cb8cbd1f8d9014564c44d120116f4990f1a6e38774c194bd1b8213286b077b0499d2e7b3f434ab12289c556684deed78131934bb3dd6537236f7c6f3dcb09d476be07721e37e1ceed9b2f7b406887bd53157305e1c8b4f84d733bc1e186fe06cc59b6edb8f4bd7ffefdf4f7ba9cfb9d570689b5a1a4109a746a690893db3799255a0cb9215d2d1cd490590e952e8c8786aa0011265252470c041dfbc3eec7c3cbf71c24869d115c0cb4a956f56d530b80ab589acfefc690751ddf36e8d383f83cedd2cc"
      },
      {
        "message": "25f10895a87716c137450bb9519dfaa1f207faa942ea88abf71e9c17980085b555aebab76264ae2a3ab93c2d12981191ddac6fb5949eb36aee3c5da940f00752c916d94608fa7d97ba6a2915b688f20323d4e9d96801d89a72ab5892dc2117c07434fcf972e058cf8c41ca4b4ff554f7d5068ad3155fced0f3125bc04f9193378a8f5c4c3b8cb4dd6d1cc69d30ecca6eaa51e36a05730e9e342e855baf099defb8afd7",
        "salt": "ad8b1523703646224b660b550885917ca2d1df28",
        "signature": "6d3b5b87f67ea657af21f75441977d2180f91b2c5f692de82955696a686730d9b9778d970758ccb26071c2209ffbd6125be2e96ea81b67cb9b9308239fda17f7b2b64ecda096b6b935640a5a1cb42a9155b1c9ef7a633a02c59f0d6ee59b852c43b35029e73c940ff0410e8f114eed46bbd0fae165e42be2528a401c3b28fd818ef3232dca9f4d2a0f5166ec59c42396d6c11dbc1215a56fa17169db9575343ef34f9de32a49cdc3174922f229c23e18e45df9353119ec4319cedce7a17c64088c1f6f52be29634100b3919d38f3d1ed94e6891e66a73b8fb849f5874df59459e298c7bbce2eee782a195aa66fe2d0732b25e595f57d3e061b1fc3e4063bf98f"
      }
    ]
  }
}`;
