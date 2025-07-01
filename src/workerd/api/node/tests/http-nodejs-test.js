import { throws, ok, strictEqual, deepStrictEqual } from 'node:assert';
import { validateHeaderName, validateHeaderValue, METHODS } from 'node:http';
import httpCommon from 'node:_http_common';
import { inspect } from 'node:util';

// Tests are taken from
// https://github.com/nodejs/node/blob/c514e8f781b2acedb6a2b42208d8f8f4d8392f09/test/parallel/test-http-header-validators.js
export const testHttpHeaderValidators = {
  async test() {
    // Expected static methods
    isFunc(validateHeaderName, 'validateHeaderName');
    isFunc(validateHeaderValue, 'validateHeaderValue');

    // - when used with valid header names - should not throw
    ['user-agent', 'USER-AGENT', 'User-Agent', 'x-forwarded-for'].forEach(
      (name) => {
        validateHeaderName(name);
      }
    );

    // - when used with invalid header names:
    ['איקס-פורוורד-פור', 'x-forwarded-fםr'].forEach((name) => {
      throws(() => validateHeaderName(name), {
        code: 'ERR_INVALID_HTTP_TOKEN',
      });
    });

    // - when used with valid header values - should not throw
    [
      ['x-valid', 1],
      ['x-valid', '1'],
      ['x-valid', 'string'],
    ].forEach(([name, value]) => {
      validateHeaderValue(name, value);
    });

    // - when used with invalid header values:
    [
      // [header, value, expectedCode]
      ['x-undefined', undefined, 'ERR_HTTP_INVALID_HEADER_VALUE'],
      ['x-bad-char', 'לא תקין', 'ERR_INVALID_CHAR'],
    ].forEach(([name, value, code]) => {
      throws(() => validateHeaderValue(name, value), { code });
    });

    // Misc.
    function isFunc(v, ttl) {
      ok(v.constructor === Function, `${ttl} is expected to be a function`);
    }
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/c514e8f781b2acedb6a2b42208d8f8f4d8392f09/test/parallel/test-http-invalidheaderfield2.js
export const testInvalidHeaderField2 = {
  async test() {
    // Good header field names
    [
      'TCN',
      'ETag',
      'date',
      'alt-svc',
      'Content-Type',
      '0',
      'Set-Cookie2',
      'Set_Cookie',
      'foo`bar^',
      'foo|bar',
      '~foobar',
      'FooBar!',
      '#Foo',
      '$et-Cookie',
      '%%Test%%',
      'Test&123',
      "It's_fun",
      '2*3',
      '4+2',
      '3.14159265359',
    ].forEach(function (str) {
      strictEqual(
        httpCommon._checkIsHttpToken(str),
        true,
        `_checkIsHttpToken(${inspect(str)}) unexpectedly failed`
      );
    });
    // Bad header field names
    [
      ':',
      '@@',
      '中文呢', // unicode
      '((((())))',
      ':alternate-protocol',
      'alternate-protocol:',
      'foo\nbar',
      'foo\rbar',
      'foo\r\nbar',
      'foo\x00bar',
      '\x7FMe!',
      '{Start',
      '(Start',
      '[Start',
      'End}',
      'End)',
      'End]',
      '"Quote"',
      'This,That',
    ].forEach(function (str) {
      strictEqual(
        httpCommon._checkIsHttpToken(str),
        false,
        `_checkIsHttpToken(${inspect(str)}) unexpectedly succeeded`
      );
    });

    // Good header field values
    [
      'foo bar',
      'foo\tbar',
      '0123456789ABCdef',
      '!@#$%^&*()-_=+\\;\':"[]{}<>,./?|~`',
    ].forEach(function (str) {
      strictEqual(
        httpCommon._checkInvalidHeaderChar(str),
        false,
        `_checkInvalidHeaderChar(${inspect(str)}) unexpectedly failed`
      );
    });

    // Bad header field values
    [
      'foo\rbar',
      'foo\nbar',
      'foo\r\nbar',
      '中文呢', // unicode
      '\x7FMe!',
      'Testing 123\x00',
      'foo\vbar',
      'Ding!\x07',
    ].forEach(function (str) {
      strictEqual(
        httpCommon._checkInvalidHeaderChar(str),
        true,
        `_checkInvalidHeaderChar(${inspect(str)}) unexpectedly succeeded`
      );
    });
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/c514e8f781b2acedb6a2b42208d8f8f4d8392f09/test/parallel/test-http-methods.js
export const testHttpMethods = {
  async test() {
    const methods = [
      'ACL',
      'BIND',
      'CHECKOUT',
      'CONNECT',
      'COPY',
      'DELETE',
      'GET',
      'HEAD',
      'LINK',
      'LOCK',
      'M-SEARCH',
      'MERGE',
      'MKACTIVITY',
      'MKCALENDAR',
      'MKCOL',
      'MOVE',
      'NOTIFY',
      'OPTIONS',
      'PATCH',
      'POST',
      'PROPFIND',
      'PROPPATCH',
      'PURGE',
      'PUT',
      'QUERY',
      'REBIND',
      'REPORT',
      'SEARCH',
      'SOURCE',
      'SUBSCRIBE',
      'TRACE',
      'UNBIND',
      'UNLINK',
      'UNLOCK',
      'UNSUBSCRIBE',
    ];

    deepStrictEqual(METHODS, methods.toSorted());
    deepStrictEqual(httpCommon.methods, methods.toSorted());
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/c514e8f781b2acedb6a2b42208d8f8f4d8392f09/test/parallel/test-http-common.js
export const testHttpCommon = {
  async test() {
    const checkIsHttpToken = httpCommon._checkIsHttpToken;
    const checkInvalidHeaderChar = httpCommon._checkInvalidHeaderChar;

    // checkIsHttpToken
    ok(checkIsHttpToken('t'));
    ok(checkIsHttpToken('tt'));
    ok(checkIsHttpToken('ttt'));
    ok(checkIsHttpToken('tttt'));
    ok(checkIsHttpToken('ttttt'));
    ok(checkIsHttpToken('content-type'));
    ok(checkIsHttpToken('etag'));

    strictEqual(checkIsHttpToken(''), false);
    strictEqual(checkIsHttpToken(' '), false);
    strictEqual(checkIsHttpToken('あ'), false);
    strictEqual(checkIsHttpToken('あa'), false);
    strictEqual(checkIsHttpToken('aaaaあaaaa'), false);

    // checkInvalidHeaderChar
    ok(checkInvalidHeaderChar('あ'));
    ok(checkInvalidHeaderChar('aaaaあaaaa'));

    strictEqual(checkInvalidHeaderChar(''), false);
    strictEqual(checkInvalidHeaderChar(1), false);
    strictEqual(checkInvalidHeaderChar(' '), false);
    strictEqual(checkInvalidHeaderChar(false), false);
    strictEqual(checkInvalidHeaderChar('t'), false);
    strictEqual(checkInvalidHeaderChar('tt'), false);
    strictEqual(checkInvalidHeaderChar('ttt'), false);
    strictEqual(checkInvalidHeaderChar('tttt'), false);
    strictEqual(checkInvalidHeaderChar('ttttt'), false);
  },
};
