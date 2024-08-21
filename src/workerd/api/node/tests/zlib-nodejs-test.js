import { strictEqual, throws, deepStrictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';
import { crc32, constants } from 'node:zlib';

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
      strictEqual(buf.length, len);
      strictEqual(
        crc32(buf, crc),
        expected,
        `crc32('${data}', ${crc}) in buffer is not ${expected}`
      );
      strictEqual(
        crc32(buf.toString(), crc),
        expected,
        `crc32('${data}', ${crc}) in string is not ${expected}`
      );
      if (crc === 0) {
        strictEqual(
          crc32(buf),
          expected,
          `crc32('${data}') in buffer is not ${expected}`
        );
        strictEqual(
          crc32(buf.toString()),
          expected,
          `crc32('${data}') in string is not ${expected}`
        );
      }
    }

    [undefined, null, true, 1, () => {}, {}].forEach((invalid) => {
      throws(
        () => {
          crc32(invalid);
        },
        { code: 'ERR_INVALID_ARG_TYPE' }
      );
    });

    [null, true, () => {}, {}].forEach((invalid) => {
      throws(
        () => {
          crc32('test', invalid);
        },
        { code: 'ERR_INVALID_ARG_TYPE' }
      );
    });
  },
};

export const constantsTest = {
  test() {
    deepStrictEqual(Object.keys(constants).sort(), [
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
