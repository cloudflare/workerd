// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'charset-parameter.window.js': {
    comment:
      'Loads each MIME type into an iframe and reads document.characterSet; workerd has no iframes or documents',
    omittedTests: true,
  },
  'parsing.any.js': {
    comment:
      'Blob/File: the test expects new Blob({type}).type to be the parsed and serialized MIME type, ' +
      'but the File API spec (and workerd, src/workerd/api/blob.c++ normalizeType) only lowercases ' +
      'printable-ASCII types and discards others, without MIME parsing. Browsers fail most of these ' +
      'too. The regexes cover the generated-mime-types.json families that fail for every listed ' +
      'code point (or for the ranges given); the strings are the failing mime-types.json cases. ' +
      'Request/Response: see the comment on each group below.',
    expectedFailures: [
      // Blob/File, generated-mime-types.json: "<c>/x" and "x/<c>" for printable non-token
      // code points. Control and non-ASCII code points pass because both sides give "".
      // U+007F would pass too if normalizeType discarded it as the File API spec requires.
      /^[ "(),:-@[\]\\{}\x7F]\/x \(Blob\/File\)$/,
      /^x\/[ "(),:-@[\]\\{}\x7F] \(Blob\/File\)$/,
      // Blob/File, generated-mime-types.json: "x/x;<c>=x;bonus=x" and "x/x;x=<c>;bonus=x"
      // only exist for code points that make the parameter invalid, and all of them fail.
      /^x\/x;[\s\S]=x;bonus=x \(Blob\/File\)$/,
      /^x\/x;x=[\s\S];bonus=x \(Blob\/File\)$/,
      // Blob/File, generated-mime-types.json: quoted parameter values containing control or
      // non-ASCII code points.
      /^x\/x;x="[^ -~]";bonus=x \(Blob\/File\)$/,
      // Blob/File, mime-types.json.
      'TEXT/HTML;CHARSET=GBK (Blob/File)',
      'text/html;charset=gbk( (Blob/File)',
      'text/html;x=(;charset=gbk (Blob/File)',
      'text/html;charset=gbk;charset=windows-1255 (Blob/File)',
      'text/html;charset=();charset=GBK (Blob/File)',
      'text/html;charset =gbk (Blob/File)',
      'text/html ;charset=gbk (Blob/File)',
      'text/html; charset=gbk (Blob/File)',
      'text/html;charset= gbk (Blob/File)',
      'text/html;charset= "gbk" (Blob/File)',
      'text/html;charset=\x0Bgbk (Blob/File)',
      'text/html;charset=\x0Cgbk (Blob/File)',
      'text/html;\x0Bcharset=gbk (Blob/File)',
      'text/html;\x0Ccharset=gbk (Blob/File)',
      "text/html;charset=';charset=GBK (Blob/File)",
      'text/html;test;charset=gbk (Blob/File)',
      'text/html;test=;charset=gbk (Blob/File)',
      "text/html;';charset=gbk (Blob/File)",
      'text/html;";charset=gbk (Blob/File)',
      'text/html ; ; charset=gbk (Blob/File)',
      'text/html;;;;charset=gbk (Blob/File)',
      'text/html;charset= "\x7F;charset=GBK (Blob/File)',
      'text/html;charset="\x7F;charset=foo";charset=GBK (Blob/File)',
      'text/html;charset="gbk" (Blob/File)',
      'text/html;charset="gbk (Blob/File)',
      'text/html;charset=gbk" (Blob/File)',
      'text/html;charset="\\ gbk" (Blob/File)',
      'text/html;charset="\\g\\b\\k" (Blob/File)',
      'text/html;charset="gbk"x (Blob/File)',
      'text/html;charset="";charset=GBK (Blob/File)',
      'text/html;charset=";charset=GBK (Blob/File)',
      'text/html;charset={gbk} (Blob/File)',
      'text/html;a]=bar;b[=bar;c=bar (Blob/File)',
      'text/html;in]valid=";asd=foo";foo=bar (Blob/File)',
      "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz/!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz;!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz=!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz (Blob/File)",
      'x/x;x="\t !\\"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8A\x8B\x8C\x8D\x8E\x8F\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9A\x9B\x9C\x9D\x9E\x9F\xA0¡¢£¤¥¦§¨©ª«¬\xAD®¯°±²³´µ¶·¸¹º»¼½¾¿ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞßàáâãäåæçèéêëìíîïðñòóôõö÷øùúûüýþÿ" (Blob/File)',
      'x/x;test (Blob/File)',
      'x/x;test="\\ (Blob/File)',
      'x/x;x=  (Blob/File)',
      'x/x;x=\t (Blob/File)',
      'x/x\n\r\t ;x=x (Blob/File)',
      '\n\r\t x/x;x=x\n\r\t  (Blob/File)',
      'x/x;\n\r\t x=x\n\r\t ;x=y (Blob/File)',
      'text/html;test=ÿ;charset=gbk (Blob/File)',
      'x/x;test=\uFFFD;x=x (Blob/File)',
      '/ (Blob/File)',
      'bogus (Blob/File)',
      'bogus/ (Blob/File)',
      'bogus/  (Blob/File)',
      'bogus/bogus/; (Blob/File)',
      '</> (Blob/File)',
      '(/) (Blob/File)',
      'text/html(;doesnot=matter (Blob/File)',
      '{/} (Blob/File)',
      'text /html (Blob/File)',
      'text/ html (Blob/File)',
      '"text/html" (Blob/File)',

      // Request/Response: workerd's MIME parser treats '\' as an HTTP token code point
      // (token_table in src/workerd/util/mimetype.c++), so it accepts '\' in types, subtypes
      // and parameter names, and serializes a '\' parameter value without quoting it.
      'x/x;test="\\ (Request/Response)',
      '\\/x (Request/Response)',
      'x/\\ (Request/Response)',
      'x/x;\\=x;bonus=x (Request/Response)',
      // Request/Response: when a parameter name is invalid, workerd's MIME parser skips to
      // the next ';' instead of first collecting the quoted-string value, so a ';' inside the
      // quotes starts a new parameter (src/workerd/util/mimetype.c++, MimeType::tryParse).
      'text/html;in]valid=";asd=foo";foo=bar (Request/Response)',
      // Request/Response: Body.blob() uses Fetch's "extract a MIME type", which splits the
      // Content-Type value on commas outside quoted strings. The test expects the whole value
      // to be parsed as one MIME type. workerd follows Fetch here.
      'x/x;,=x;bonus=x (Request/Response)',
      'x/x;x=,;bonus=x (Request/Response)',
      // Request/Response: workerd deliberately accepts header values with code points above
      // U+00FF (encoding them as UTF-8 and logging a warning) instead of throwing a TypeError
      // (src/workerd/api/headers.c++, maybeWarnIfBadHeaderString).
      'x/x;test=\uFFFD;x=x (Request/Response)',
      'Ā/Ā (Request/Response)',
    ],
  },
} satisfies TestRunnerConfig;
