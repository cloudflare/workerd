import {
  deepStrictEqual,
  strictEqual,
  ok,
  fail,
  throws,
} from 'node:assert';

export const constructAndGet = {
  test() {

    const cases = [
      {
        'input': 'https://capnproto.org',
        'pathname': '/',
        'comment': 'special URL, 0-component path'
      },
      {
        'input': 'https://capnproto.org/',
        'pathname': '/',
        'comment': 'special URL, 1-component path with empty terminal component'
      },
      {
        'input': 'https://capnproto.org/foo',
        'pathname': '/foo',
        'comment': 'special URL, 1-component path'
      },
      {
        'input': 'https://capnproto.org/foo/',
        'pathname': '/foo/',
        'comment': 'special URL, 2-component path with empty terminal component'
      },
      {
       'input': 'https://capnproto.org/%2F',
       'pathname': '/%2F',
       'comment': 'encoded slash can be a path component'
      },
      {
       'input': 'https://capnproto.org//',
       'pathname': '//',
       'comment': '// in path is not collapsed'
      },
      {
        'input': 'https://capnproto.org/./',
        'pathname': '/',
        'comment': '/./ in path is collapsed'
      },
      {
        'input': 'https://capnproto.org/?<>',
        'search': '?%3C%3E',
        'comment': 'angle brackets in search get percent-encoded'
      },
      {
       'input': 'https://capnproto.org/??',
       'search': '??',
       'comment': 'question mark in search does not get percent-encoded'
      },
      {
        'input': 'https://capnproto.org/?foo',
        'search': '?foo',
        'href': 'https://capnproto.org/?foo',
        'comment': 'No-valued/empty-valued query parameters are preserved round-trip'
      },
      {
        'input': 'https://capnproto.org/?foo=',
        'search': '?foo=',
        'href': 'https://capnproto.org/?foo=',
        'comment': 'No-valued/empty-valued query parameters are preserved round-trip'
      },
      {
        'input': 'https://capnproto.org/?foo=&bar',
        'search': '?foo=&bar',
        'href': 'https://capnproto.org/?foo=&bar',
        'comment': 'No-valued/empty-valued query parameters are preserved round-trip'
      },
      {
        'input': 'https://capnproto.org/?foo&bar=',
        'search': '?foo&bar=',
        'href': 'https://capnproto.org/?foo&bar=',
        'comment': 'No-valued/empty-valued query parameters are preserved round-trip'
      },
      {
        'input': 'https://capnproto.org/?foo+bar=baz+qux',
        'search': '?foo+bar=baz+qux',
        'comment': 'pluses in search do not get percent-encoded'
      },
      {
        'input': 'https://capnproto.org/?😺',
        'search': '?%F0%9F%98%BA',
        'comment': 'cat emoji in search gets percent-encoded'
      },
      {
        'input': 'https://capnproto.org/#😺',
        'hash': '#%F0%9F%98%BA',
        'comment': 'cat emoji in hash gets percent-encoded'
      },
      {
        'input': 'https://capnproto.org:443/',
        'href': 'https://capnproto.org/',
        'comment': 'Parsing a URL with an explicit scheme-default port ignores the port'
      },
      {
        'input': 'https://capnproto.org:/',
        'href': 'https://capnproto.org/',
        'comment': 'Parsing a URL with a ":" but no port ignores port'
      },
      {
        'input': 'http://foo/bar;baz@qux',
        'href': 'http://foo/bar;baz@qux',
        'comment': 'URL path components are encoded with the path percent encode set'
      },
      {
        'input': 'https://jam_com.helpusersvote.net/',
        'href': 'https://jam_com.helpusersvote.net/',
        'comment': 'Underscores are allowed in hostnames'
      },
      {
        'input': 'https://foo%25bar:baz%25qux@capnproto.org/foo%25bar?foo%25bar=baz%25qux#foo%25bar',
        'href': 'https://foo%25bar:baz%25qux@capnproto.org/foo%25bar?foo%25bar=baz%25qux#foo%25bar',
        'comment': 'Encoded percent signs in userinfo, path, query, and fragment get round-tripped'
      },
    ];

    cases.forEach((test) => {
      ['pathname', 'search', 'hash', 'href'].forEach((attribute) => {
        const url = new URL(test.input);
        if (test[attribute] !== undefined) {
          strictEqual(url[attribute], test[attribute], `${test.input}: ${test.comment}`);
        }
      });
    });
  }
};

export const constructSetAndGet = {
  test() {
    const cases = {
      'protocol': [
        {
          'href': 'https://example.com/',
          'new_value': 'http',
          'expected': {
            'href': 'http://example.com/'
          },
          'comment': 'Setting scheme on URL with null port does not change port'
        },
        {
          'href': 'https://example.com:80/',
          'new_value': 'http',
          'expected': {
            'href': 'http://example.com/'
          },
          'comment': 'Setting scheme on URL with port that is the new scheme-default port nulls out port'
        },
        {
          'href': 'https://example.com:1234/',
          'new_value': 'http',
          'expected': {
            'href': 'http://example.com:1234/'
          },
          'comment': 'Setting scheme on URL with non-scheme-default port preserves port'
        },
      ],
      'search': [
        {
          'href': 'https://capnproto.org/',
          'new_value': '#',
          'expected': {
            'href': 'https://capnproto.org/?%23',
            'search': '?%23'
          }
        },
        {
          'href': 'https://capnproto.org/',
          'new_value': '#=#',
          'expected': {
            'href': 'https://capnproto.org/?%23=%23',
            'search': '?%23=%23'
          }
        }
      ],
      'host': [
        {
          'href': 'https://example.com/',
          'new_value': 'foo.example.com:',
          'expected': {
            'href': 'https://foo.example.com/'
          },
          'comment': 'Setting host with ":" but no port ignores port'
        },
        {
          'href': 'https://example.com/',
          'new_value': 'foo.example.com:80',
          'expected': {
            'href': 'https://foo.example.com:80/'
          },
          'comment': 'Setting host with non-scheme-default port sets port'
        },
        {
          'href': 'https://example.com:1234/',
          'new_value': 'foo.example.com:443',
          'expected': {
            'href': 'https://foo.example.com/'
          },
          'comment': 'Setting host with scheme-default port nulls out port'
        },
        {
          'href': 'https://example.com/',
          'new_value': 'foo.example.com:443',
          'expected': {
            'href': 'https://foo.example.com/'
          },
          'comment': 'Setting host with scheme-default port nulls out port'
        },
      ],
      'port': [
        {
          'href': 'https://example.com/',
          'new_value': '443',
          'expected': {
            'href': 'https://example.com/'
          },
          'comment': 'Setting port to scheme-default port is a no-op'
        },
        {
          'href': 'https://example.com:1234/',
          'new_value': '443',
          'expected': {
            'href': 'https://example.com/'
          },
          'comment': 'Setting port to scheme-default port nulls out port'
        },
        {
          'href': 'https://example.com/',
          'new_value': '1234',
          'expected': {
            'href': 'https://example.com:1234/'
          },
          'comment': 'Setting port to non-scheme-default port adopts port'
        },
        {
          'href': 'https://example.com:80/',
          'new_value': '1234',
          'expected': {
            'href': 'https://example.com:1234/'
          },
          'comment': 'Setting port to non-scheme-default port adopts port'
        },
        {
          'href': 'https://example.com:1234/',
          'new_value': '',
          'expected': {
            'href': 'https://example.com/'
          },
          'comment': 'Setting port to the empty string nulls out port'
        },
      ],
      'hostname': [
        {
          'href': 'http://example.com/path/to/something?query=foo#hash',
          'new_value': 'newexample.com',
          'expected': {
            'href': 'http://newexample.com/path/to/something?query=foo#hash'
          }
        }
      ]
    };

    for (const attribute in cases) {
      cases[attribute].forEach((test) => {
        const url = new URL(test.href);
        url[attribute] = test.new_value;
        for (const a in test.expected) {
          strictEqual(url[a], test.expected[a], `${test.href}: ${test.comment}`);
        }
      });
    }
  }
};

export const urlSearchParamsStringifyBehavior = {
  test() {
    const url = new URL('https://example.com/?foo&bar=&baz=qux');
    strictEqual(url.href,'https://example.com/?foo&bar=&baz=qux');
    strictEqual(url.search, '?foo&bar=&baz=qux');
    strictEqual(url.searchParams.toString(), 'foo=&bar=&baz=qux');
  }
};

export const urlSearchParamsForEach = {
  test() {
    let searchParams = new URLSearchParams('');
    searchParams.forEach(() => {
      fail('Should not have been called');
    });

    const foreachOutput = [];
    searchParams = new URLSearchParams('key1=value1&key2=value2');
    strictEqual(searchParams.size, 2);
    let pushed = false;
    searchParams.forEach((val, key) => {
      foreachOutput.push([key,val]);
      if (!pushed) {
        // We can add params within this loop but it's not really safe
        // because it will cause the loop to run forever if we're not
        // careful.
        pushed = true;
        searchParams.set('key3', 'value3');
      }
    });
    deepStrictEqual(foreachOutput, [['key1','value1'],
                                    ['key2','value2'],
                                    ['key3','value3']]);
    strictEqual(searchParams.size, 3);

    // Calling forEach with no argument throws
    throws(() => searchParams.forEach());

    // Calling forEach with wrong arguments throws
    throws(() => searchParams.forEach(1));

    // This can be overridden by the second argument
    searchParams.forEach(function() {
      strictEqual(this, 1);
    }, 1);

    // Propagates errors
    throws(() => searchParams.forEach(() => {
      throw new Error('boom');
    }));
  }
};

export const urlSearchParamsInit1 = {
  test() {
    const search1 = new URLSearchParams('a=b');
    const search2 = new URLSearchParams(search1);
    strictEqual(search1.toString(), search2.toString());
  }
};

export const urlSearchParamsInit2 = {
  test() {
    const search1 = new URLSearchParams('a=b');
    search1[Symbol.iterator] = function* () {
      yield ['y', 'z'];
    };
    const search2 = new URLSearchParams(search1);
    ok(!search2.has('a'));
    ok(search2.has('y'));
    strictEqual(search2.get('y'), 'z');
  }
};

export const urlSearchParamsInit3 = {
  test() {
    // If the initializer has a deleted iterator, then it's
    // contents are ignored but can still be interpreted as
    // a dictionary.
    const search1 = new URLSearchParams('a=b');
    search1[Symbol.iterator] = undefined;
    search1.y = 'z';
    const search2 = new URLSearchParams(search1);
    ok(!search2.has('a'));
    ok(search2.has('y'));
    strictEqual(search2.get('y'), 'z');
  }
};

export const urlSearchParamsInit4 = {
  test() {
    const formdata = new FormData();
    formdata.append('a', 'b');
    ok(formdata.has('a'));
    const search2 = new URLSearchParams(formdata);
    ok(search2.has('a'));
    strictEqual(search2.get('a'), 'b');
  }
};

export const urlSearchParamsInit5 = {
  test() {
    const formdata = new FormData();
    formdata.append('a', 'b');
    formdata[Symbol.iterator] = undefined;
    const search2 = new URLSearchParams(formdata);
    ok(!search2.has('a'));
  }
};

export const urlToJson = {
  test() {
    const url = new URL('https://example.com');
    strictEqual(JSON.stringify(url), '\"https://example.com/\"');
  }
};

export const urlSearchParamsSet = {
  test() {
    const url = new URL('https://example.com?c=d');
    url.searchParams.set('a', 'b');
    strictEqual(url.searchParams.size, 2);
    url.searchParams.delete('c');
    strictEqual(url.searchParams.size, 1);
    ok(url.searchParams.has('a'));
    ok(!url.searchParams.has('c'));
    ok(url.searchParams.has('a', 'b'));
    ok(!url.searchParams.has('a', 'c'));
    url.searchParams.delete('a', 'c');
    ok(url.searchParams.has('a'));
  }
};

export const urlConstructorTests = {
  test() {
    const cases = [
      "# Based on http://trac.webkit.org/browser/trunk/LayoutTests/fast/url/script-tests/segments.js",
      {
       "input": "http://example\t.\norg",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
        "input": "C|/",
        "base": "file://host/dir/file",
        "href": "file://host/C:/",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "host",
        "hostname": "host",
        "port": "",
        "pathname": "/C:/",
        "search": "",
        "hash": ""
      },
      {
       "input": "C|\n/",
       "base": "file://host/dir/file",
       "href": "file://host/C:/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "host",
       "hostname": "host",
       "port": "",
       "pathname": "/C:/",
       "search": "",
       "hash": ""
      },
      {
       "input": "https://:@test",
       "base": "about:blank",
       "href": "https://test/",
       "origin": "https://test",
       "protocol": "https:",
       "username": "",
       "password": "",
       "host": "test",
       "hostname": "test",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "non-special://test:@test/x",
       "base": "about:blank",
       "href": "non-special://test@test/x",
       "origin": "null",
       "protocol": "non-special:",
       "username": "test",
       "password": "",
       "host": "test",
       "hostname": "test",
       "port": "",
       "pathname": "/x",
       "search": "",
       "hash": ""
      },
      {
       "input": "non-special://:@test/x",
       "base": "about:blank",
       "href": "non-special://test/x",
       "origin": "null",
       "protocol": "non-special:",
       "username": "",
       "password": "",
       "host": "test",
       "hostname": "test",
       "port": "",
       "pathname": "/x",
       "search": "",
       "hash": ""
      },
      {
        "input": "http:foo.com",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/foo/foo.com",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/foo/foo.com",
        "search": "",
        "hash": ""
      },
      {
       "input": "\t   :foo.com   \n",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/:foo.com",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/:foo.com",
       "search": "",
       "hash": ""
      },
      {
       "input": " foo.com  ",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/foo.com",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/foo.com",
       "search": "",
       "hash": ""
      },
      {
       "input": "a:\t foo.com",
       "base": "http://example.org/foo/bar",
       "href": "a: foo.com",
       "origin": "null",
       "protocol": "a:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": " foo.com",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://f:21/ b ? d # e ",
       "base": "http://example.org/foo/bar",
       "href": "http://f:21/%20b%20?%20d%20#%20e",
       "origin": "http://f:21",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "f:21",
       "hostname": "f",
       "port": "21",
       "pathname": "/%20b%20",
       "search": "?%20d%20",
       "hash": "#%20e"
      },
      {
       "input": "lolscheme:x x#x x",
       "base": "about:blank",
       "href": "lolscheme:x x#x%20x",
       "protocol": "lolscheme:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "x x",
       "search": "",
       "hash": "#x%20x"
      },
      {
       "input": "http://f:/c",
       "base": "http://example.org/foo/bar",
       "href": "http://f/c",
       "origin": "http://f",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "f",
       "hostname": "f",
       "port": "",
       "pathname": "/c",
       "search": "",
       "hash": ""
      },
      {
        "input": "http://f:0/c",
        "base": "http://example.org/foo/bar",
        "href": "http://f:0/c",
        "origin": "http://f:0",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "f:0",
        "hostname": "f",
        "port": "0",
        "pathname": "/c",
        "search": "",
        "hash": ""
      },
      {
       "input": "http://f:00000000000000/c",
       "base": "http://example.org/foo/bar",
       "href": "http://f:0/c",
       "origin": "http://f:0",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "f:0",
       "hostname": "f",
       "port": "0",
       "pathname": "/c",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://f:00000000000000000000080/c",
       "base": "http://example.org/foo/bar",
       "href": "http://f/c",
       "origin": "http://f",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "f",
       "hostname": "f",
       "port": "",
       "pathname": "/c",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://f:b/c",
       "base": "http://example.org/foo/bar",
       "failure": true
      },
      {
       "input": "http://f: /c",
       "base": "http://example.org/foo/bar",
       "failure": true
      },
      {
       "input": "http://f:\n/c",
       "base": "http://example.org/foo/bar",
       "href": "http://f/c",
       "origin": "http://f",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "f",
       "hostname": "f",
       "port": "",
       "pathname": "/c",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://f:fifty-two/c",
       "base": "http://example.org/foo/bar",
       "failure": true
      },
      {
       "input": "http://f:999999/c",
       "base": "http://example.org/foo/bar",
       "failure": true
      },
      {
       "input": "non-special://f:999999/c",
       "base": "http://example.org/foo/bar",
       "failure": true
      },
      {
       "input": "http://f: 21 / b ? d # e ",
       "base": "http://example.org/foo/bar",
       "failure": true
      },
      {
        "input": "",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/foo/bar",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/foo/bar",
        "search": "",
        "hash": ""
      },
      {
       "input": "  \t",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/bar",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/bar",
       "search": "",
       "hash": ""
      },
      {
       "input": ":foo.com/",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/:foo.com/",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/:foo.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": ":foo.com\\",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/:foo.com/",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/:foo.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": ":",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/:",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/:",
       "search": "",
       "hash": ""
      },
      {
       "input": ":a",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/:a",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/:a",
       "search": "",
       "hash": ""
      },
      {
       "input": ":/",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/:/",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/:/",
       "search": "",
       "hash": ""
      },
      {
       "input": ":\\",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/:/",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/:/",
       "search": "",
       "hash": ""
      },
      {
       "input": ":#",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/:#",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/:",
       "search": "",
       "hash": ""
      },
      {
        "input": "#",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/foo/bar#",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/foo/bar",
        "search": "",
        "hash": ""
      },
      {
        "input": "#/",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/foo/bar#/",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/foo/bar",
        "search": "",
        "hash": "#/"
      },
      {
        "input": "#\\",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/foo/bar#\\",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/foo/bar",
        "search": "",
        "hash": "#\\"
      },
      {
        "input": "#;?",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/foo/bar#;?",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/foo/bar",
        "search": "",
        "hash": "#;?"
      },
      {
       "input": "?",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/bar?",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/bar",
       "search": "",
       "hash": ""
      },
      {
        "input": "/",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/",
        "search": "",
        "hash": ""
      },
      {
       "input": ":23",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/:23",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/:23",
       "search": "",
       "hash": ""
      },
      {
        "input": "/:23",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/:23",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/:23",
        "search": "",
        "hash": ""
      },
      {
       "input": "::",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/::",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/::",
       "search": "",
       "hash": ""
      },
      {
       "input": "::23",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/::23",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/::23",
       "search": "",
       "hash": ""
      },
      {
        "input": "foo://",
        "base": "http://example.org/foo/bar",
        "href": "foo://",
        "origin": "null",
        "protocol": "foo:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "",
        "search": "",
        "hash": ""
      },
      {
        "input": "http://a:b@c:29/d",
        "base": "http://example.org/foo/bar",
        "href": "http://a:b@c:29/d",
        "origin": "http://c:29",
        "protocol": "http:",
        "username": "a",
        "password": "b",
        "host": "c:29",
        "hostname": "c",
        "port": "29",
        "pathname": "/d",
        "search": "",
        "hash": ""
      },
      {
       "input": "http::@c:29",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/:@c:29",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/:@c:29",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://&a:foo(b]c@d:2/",
       "base": "http://example.org/foo/bar",
       "href": "http://&a:foo(b%5Dc@d:2/",
       "origin": "http://d:2",
       "protocol": "http:",
       "username": "&a",
       "password": "foo(b%5Dc",
       "host": "d:2",
       "hostname": "d",
       "port": "2",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://::@c@d:2",
       "base": "http://example.org/foo/bar",
       "href": "http://:%3A%40c@d:2/",
       "origin": "http://d:2",
       "protocol": "http:",
       "username": "",
       "password": "%3A%40c",
       "host": "d:2",
       "hostname": "d",
       "port": "2",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
        "input": "http://foo.com:b@d/",
        "base": "http://example.org/foo/bar",
        "href": "http://foo.com:b@d/",
        "origin": "http://d",
        "protocol": "http:",
        "username": "foo.com",
        "password": "b",
        "host": "d",
        "hostname": "d",
        "port": "",
        "pathname": "/",
        "search": "",
        "hash": ""
      },
      {
       "input": "http://foo.com/\\@",
       "base": "http://example.org/foo/bar",
       "href": "http://foo.com//@",
       "origin": "http://foo.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "foo.com",
       "hostname": "foo.com",
       "port": "",
       "pathname": "//@",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:\\\\foo.com\\",
       "base": "http://example.org/foo/bar",
       "href": "http://foo.com/",
       "origin": "http://foo.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "foo.com",
       "hostname": "foo.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:\\\\a\\b:c\\d@foo.com\\",
       "base": "http://example.org/foo/bar",
       "href": "http://a/b:c/d@foo.com/",
       "origin": "http://a",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "a",
       "hostname": "a",
       "port": "",
       "pathname": "/b:c/d@foo.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "foo:/",
       "base": "http://example.org/foo/bar",
       "href": "foo:/",
       "origin": "null",
       "protocol": "foo:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "foo:/bar.com/",
       "base": "http://example.org/foo/bar",
       "href": "foo:/bar.com/",
       "origin": "null",
       "protocol": "foo:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/bar.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "foo://///////",
       "base": "http://example.org/foo/bar",
       "href": "foo://///////",
       "origin": "null",
       "protocol": "foo:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "///////",
       "search": "",
       "hash": ""
      },
      {
       "input": "foo://///////bar.com/",
       "base": "http://example.org/foo/bar",
       "href": "foo://///////bar.com/",
       "origin": "null",
       "protocol": "foo:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "///////bar.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "foo:////://///",
       "base": "http://example.org/foo/bar",
       "href": "foo:////://///",
       "origin": "null",
       "protocol": "foo:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "//://///",
       "search": "",
       "hash": ""
      },
      {
       "input": "c:/foo",
       "base": "http://example.org/foo/bar",
       "href": "c:/foo",
       "origin": "null",
       "protocol": "c:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/foo",
       "search": "",
       "hash": ""
      },
      {
        "input": "//foo/bar",
        "base": "http://example.org/foo/bar",
        "href": "http://foo/bar",
        "origin": "http://foo",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "foo",
        "hostname": "foo",
        "port": "",
        "pathname": "/bar",
        "search": "",
        "hash": ""
      },
      {
       "input": "http://foo/path;a??e#f#g",
       "base": "http://example.org/foo/bar",
       "href": "http://foo/path;a??e#f#g",
       "origin": "http://foo",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "foo",
       "hostname": "foo",
       "port": "",
       "pathname": "/path;a",
       "search": "??e",
       "hash": "#f#g"
      },
      {
       "input": "http://foo/abcd?efgh?ijkl",
       "base": "http://example.org/foo/bar",
       "href": "http://foo/abcd?efgh?ijkl",
       "origin": "http://foo",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "foo",
       "hostname": "foo",
       "port": "",
       "pathname": "/abcd",
       "search": "?efgh?ijkl",
       "hash": ""
      },
      {
        "input": "http://foo/abcd#foo?bar",
        "base": "http://example.org/foo/bar",
        "href": "http://foo/abcd#foo?bar",
        "origin": "http://foo",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "foo",
        "hostname": "foo",
        "port": "",
        "pathname": "/abcd",
        "search": "",
        "hash": "#foo?bar"
      },
      {
       "input": "[61:24:74]:98",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/[61:24:74]:98",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/[61:24:74]:98",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:[61:27]/:foo",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/[61:27]/:foo",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/[61:27]/:foo",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://[1::2]:3:4",
       "base": "http://example.org/foo/bar",
       "failure": true
      },
      {
       "input": "http://2001::1",
       "base": "http://example.org/foo/bar",
       "failure": true
      },
      {
       "input": "http://2001::1]",
       "base": "http://example.org/foo/bar",
       "failure": true
      },
      {
       "input": "http://2001::1]:80",
       "base": "http://example.org/foo/bar",
       "failure": true
      },
      {
       "input": "http://[2001::1]",
       "base": "http://example.org/foo/bar",
       "href": "http://[2001::1]/",
       "origin": "http://[2001::1]",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "[2001::1]",
       "hostname": "[2001::1]",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://[::127.0.0.1]",
       "base": "http://example.org/foo/bar",
       "href": "http://[::7f00:1]/",
       "origin": "http://[::7f00:1]",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "[::7f00:1]",
       "hostname": "[::7f00:1]",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://[0:0:0:0:0:0:13.1.68.3]",
       "base": "http://example.org/foo/bar",
       "href": "http://[::d01:4403]/",
       "origin": "http://[::d01:4403]",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "[::d01:4403]",
       "hostname": "[::d01:4403]",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://[2001::1]:80",
       "base": "http://example.org/foo/bar",
       "href": "http://[2001::1]/",
       "origin": "http://[2001::1]",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "[2001::1]",
       "hostname": "[2001::1]",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
        "input": "http:/example.com/",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/example.com/",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/example.com/",
        "search": "",
        "hash": ""
      },
      {
       "input": "ftp:/example.com/",
       "base": "http://example.org/foo/bar",
       "href": "ftp://example.com/",
       "origin": "ftp://example.com",
       "protocol": "ftp:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "https:/example.com/",
       "base": "http://example.org/foo/bar",
       "href": "https://example.com/",
       "origin": "https://example.com",
       "protocol": "https:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "madeupscheme:/example.com/",
       "base": "http://example.org/foo/bar",
       "href": "madeupscheme:/example.com/",
       "origin": "null",
       "protocol": "madeupscheme:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "file:/example.com/",
       "base": "http://example.org/foo/bar",
       "href": "file:///example.com/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
        "input": "file://example:1/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "file://example:test/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "file://example%/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "file://[example]/",
        "base": "about:blank",
        "failure": true
      },
      {
       "input": "ftps:/example.com/",
       "base": "http://example.org/foo/bar",
       "href": "ftps:/example.com/",
       "origin": "null",
       "protocol": "ftps:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "gopher:/example.com/",
       "base": "http://example.org/foo/bar",
       "href": "gopher:/example.com/",
       "protocol": "gopher:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ws:/example.com/",
       "base": "http://example.org/foo/bar",
       "href": "ws://example.com/",
       "origin": "ws://example.com",
       "protocol": "ws:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "wss:/example.com/",
       "base": "http://example.org/foo/bar",
       "href": "wss://example.com/",
       "origin": "wss://example.com",
       "protocol": "wss:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "data:/example.com/",
       "base": "http://example.org/foo/bar",
       "href": "data:/example.com/",
       "origin": "null",
       "protocol": "data:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "javascript:/example.com/",
       "base": "http://example.org/foo/bar",
       "href": "javascript:/example.com/",
       "origin": "null",
       "protocol": "javascript:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "mailto:/example.com/",
       "base": "http://example.org/foo/bar",
       "href": "mailto:/example.com/",
       "origin": "null",
       "protocol": "mailto:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
        "input": "http:example.com/",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/foo/example.com/",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/foo/example.com/",
        "search": "",
        "hash": ""
      },
      {
       "input": "ftp:example.com/",
       "base": "http://example.org/foo/bar",
       "href": "ftp://example.com/",
       "origin": "ftp://example.com",
       "protocol": "ftp:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "https:example.com/",
       "base": "http://example.org/foo/bar",
       "href": "https://example.com/",
       "origin": "https://example.com",
       "protocol": "https:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "madeupscheme:example.com/",
       "base": "http://example.org/foo/bar",
       "href": "madeupscheme:example.com/",
       "origin": "null",
       "protocol": "madeupscheme:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ftps:example.com/",
       "base": "http://example.org/foo/bar",
       "href": "ftps:example.com/",
       "origin": "null",
       "protocol": "ftps:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "gopher:example.com/",
       "base": "http://example.org/foo/bar",
       "href": "gopher:example.com/",
       "protocol": "gopher:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ws:example.com/",
       "base": "http://example.org/foo/bar",
       "href": "ws://example.com/",
       "origin": "ws://example.com",
       "protocol": "ws:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "wss:example.com/",
       "base": "http://example.org/foo/bar",
       "href": "wss://example.com/",
       "origin": "wss://example.com",
       "protocol": "wss:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "data:example.com/",
       "base": "http://example.org/foo/bar",
       "href": "data:example.com/",
       "origin": "null",
       "protocol": "data:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "javascript:example.com/",
       "base": "http://example.org/foo/bar",
       "href": "javascript:example.com/",
       "origin": "null",
       "protocol": "javascript:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "mailto:example.com/",
       "base": "http://example.org/foo/bar",
       "href": "mailto:example.com/",
       "origin": "null",
       "protocol": "mailto:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "example.com/",
       "search": "",
       "hash": ""
      },
      {
        "input": "/a/b/c",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/a/b/c",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/a/b/c",
        "search": "",
        "hash": ""
      },
      {
        "input": "/a/ /c",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/a/%20/c",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/a/%20/c",
        "search": "",
        "hash": ""
      },
      {
       "input": "/a%2fc",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/a%2fc",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/a%2fc",
       "search": "",
       "hash": ""
      },
      {
       "input": "/a/%2f/c",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/a/%2f/c",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/a/%2f/c",
       "search": "",
       "hash": ""
      },
      {
        "input": "#β",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/foo/bar#%CE%B2",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/foo/bar",
        "search": "",
        "hash": "#%CE%B2"
      },
      {
       "input": "data:text/html,test#test",
       "base": "http://example.org/foo/bar",
       "href": "data:text/html,test#test",
       "origin": "null",
       "protocol": "data:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "text/html,test",
       "search": "",
       "hash": "#test"
      },
      {
       "input": "tel:1234567890",
       "base": "http://example.org/foo/bar",
       "href": "tel:1234567890",
       "origin": "null",
       "protocol": "tel:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "1234567890",
       "search": "",
       "hash": ""
      },
      "# Based on http://trac.webkit.org/browser/trunk/LayoutTests/fast/url/file.html",
      {
       "input": "file:c:\\foo\\bar.html",
       "base": "file:///tmp/mock/path",
       "href": "file:///c:/foo/bar.html",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/c:/foo/bar.html",
       "search": "",
       "hash": ""
      },
      {
       "input": "  File:c|////foo\\bar.html",
       "base": "file:///tmp/mock/path",
       "href": "file:///c:////foo/bar.html",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/c:////foo/bar.html",
       "search": "",
       "hash": ""
      },
      {
       "input": "C|/foo/bar",
       "base": "file:///tmp/mock/path",
       "href": "file:///C:/foo/bar",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/C:/foo/bar",
       "search": "",
       "hash": ""
      },
      {
       "input": "/C|\\foo\\bar",
       "base": "file:///tmp/mock/path",
       "href": "file:///C:/foo/bar",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/C:/foo/bar",
       "search": "",
       "hash": ""
      },
      {
       "input": "//C|/foo/bar",
       "base": "file:///tmp/mock/path",
       "href": "file:///C:/foo/bar",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/C:/foo/bar",
       "search": "",
       "hash": ""
      },
      {
        "input": "//server/file",
        "base": "file:///tmp/mock/path",
        "href": "file://server/file",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "server",
        "hostname": "server",
        "port": "",
        "pathname": "/file",
        "search": "",
        "hash": ""
      },
      {
       "input": "\\\\server\\file",
       "base": "file:///tmp/mock/path",
       "href": "file://server/file",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "server",
       "hostname": "server",
       "port": "",
       "pathname": "/file",
       "search": "",
       "hash": ""
      },
      {
       "input": "/\\server/file",
       "base": "file:///tmp/mock/path",
       "href": "file://server/file",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "server",
       "hostname": "server",
       "port": "",
       "pathname": "/file",
       "search": "",
       "hash": ""
      },
      {
        "input": "file:///foo/bar.txt",
        "base": "file:///tmp/mock/path",
        "href": "file:///foo/bar.txt",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/foo/bar.txt",
        "search": "",
        "hash": ""
      },
      {
        "input": "file:///home/me",
        "base": "file:///tmp/mock/path",
        "href": "file:///home/me",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/home/me",
        "search": "",
        "hash": ""
      },
      {
       "input": "//",
       "base": "file:///tmp/mock/path",
       "href": "file:///",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
        "input": "///",
        "base": "file:///tmp/mock/path",
        "href": "file:///",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/",
        "search": "",
        "hash": ""
      },
      {
        "input": "///test",
        "base": "file:///tmp/mock/path",
        "href": "file:///test",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/test",
        "search": "",
        "hash": ""
      },
      {
       "input": "file://test",
       "base": "file:///tmp/mock/path",
       "href": "file://test/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "test",
       "hostname": "test",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "file://localhost",
       "base": "file:///tmp/mock/path",
       "href": "file:///",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "file://localhost/",
       "base": "file:///tmp/mock/path",
       "href": "file:///",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "file://localhost/test",
       "base": "file:///tmp/mock/path",
       "href": "file:///test",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/test",
       "search": "",
       "hash": ""
      },
      {
        "input": "test",
        "base": "file:///tmp/mock/path",
        "href": "file:///tmp/mock/test",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/tmp/mock/test",
        "search": "",
        "hash": ""
      },
      {
        "input": "file:test",
        "base": "file:///tmp/mock/path",
        "href": "file:///tmp/mock/test",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/tmp/mock/test",
        "search": "",
        "hash": ""
      },
      "# Based on http://trac.webkit.org/browser/trunk/LayoutTests/fast/url/script-tests/path.js",
      {
       "input": "http://example.com/././foo",
       "base": "about:blank",
       "href": "http://example.com/foo",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/./.foo",
       "base": "about:blank",
       "href": "http://example.com/.foo",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/.foo",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/.",
       "base": "about:blank",
       "href": "http://example.com/foo/",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/./",
       "base": "about:blank",
       "href": "http://example.com/foo/",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/bar/..",
       "base": "about:blank",
       "href": "http://example.com/foo/",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/bar/../",
       "base": "about:blank",
       "href": "http://example.com/foo/",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/..bar",
       "base": "about:blank",
       "href": "http://example.com/foo/..bar",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo/..bar",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/bar/../ton",
       "base": "about:blank",
       "href": "http://example.com/foo/ton",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo/ton",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/bar/../ton/../../a",
       "base": "about:blank",
       "href": "http://example.com/a",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/a",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/../../..",
       "base": "about:blank",
       "href": "http://example.com/",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/../../../ton",
       "base": "about:blank",
       "href": "http://example.com/ton",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/ton",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/%2e",
       "base": "about:blank",
       "href": "http://example.com/foo/",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/%2e%2",
       "base": "about:blank",
       "href": "http://example.com/foo/%2e%2",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo/%2e%2",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/%2e./%2e%2e/.%2e/%2e.bar",
       "base": "about:blank",
       "href": "http://example.com/%2e.bar",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/%2e.bar",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com////../..",
       "base": "about:blank",
       "href": "http://example.com//",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "//",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/bar//../..",
       "base": "about:blank",
       "href": "http://example.com/foo/",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo/bar//..",
       "base": "about:blank",
       "href": "http://example.com/foo/bar/",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo/bar/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo",
       "base": "about:blank",
       "href": "http://example.com/foo",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/%20foo",
       "base": "about:blank",
       "href": "http://example.com/%20foo",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/%20foo",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo%",
       "base": "about:blank",
       "href": "http://example.com/foo%",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo%",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo%2",
       "base": "about:blank",
       "href": "http://example.com/foo%2",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo%2",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo%2zbar",
       "base": "about:blank",
       "href": "http://example.com/foo%2zbar",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo%2zbar",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo%2Â©zbar",
       "base": "about:blank",
       "href": "http://example.com/foo%2%C3%82%C2%A9zbar",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo%2%C3%82%C2%A9zbar",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo%41%7a",
       "base": "about:blank",
       "href": "http://example.com/foo%41%7a",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo%41%7a",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo\t\u0091%91",
       "base": "about:blank",
       "href": "http://example.com/foo%C2%91%91",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo%C2%91%91",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo%00%51",
       "base": "about:blank",
       "href": "http://example.com/foo%00%51",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foo%00%51",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/(%28:%3A%29)",
       "base": "about:blank",
       "href": "http://example.com/(%28:%3A%29)",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/(%28:%3A%29)",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/%3A%3a%3C%3c",
       "base": "about:blank",
       "href": "http://example.com/%3A%3a%3C%3c",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/%3A%3a%3C%3c",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/foo\tbar",
       "base": "about:blank",
       "href": "http://example.com/foobar",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/foobar",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com\\\\foo\\\\bar",
       "base": "about:blank",
       "href": "http://example.com//foo//bar",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "//foo//bar",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/%7Ffp3%3Eju%3Dduvgw%3Dd",
       "base": "about:blank",
       "href": "http://example.com/%7Ffp3%3Eju%3Dduvgw%3Dd",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/%7Ffp3%3Eju%3Dduvgw%3Dd",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/@asdf%40",
       "base": "about:blank",
       "href": "http://example.com/@asdf%40",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/@asdf%40",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/你好你好",
       "base": "about:blank",
       "href": "http://example.com/%E4%BD%A0%E5%A5%BD%E4%BD%A0%E5%A5%BD",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/%E4%BD%A0%E5%A5%BD%E4%BD%A0%E5%A5%BD",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/‥/foo",
       "base": "about:blank",
       "href": "http://example.com/%E2%80%A5/foo",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/%E2%80%A5/foo",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/﻿/foo",
       "base": "about:blank",
       "href": "http://example.com/%EF%BB%BF/foo",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/%EF%BB%BF/foo",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.com/‮/foo/‭/bar",
       "base": "about:blank",
       "href": "http://example.com/%E2%80%AE/foo/%E2%80%AD/bar",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/%E2%80%AE/foo/%E2%80%AD/bar",
       "search": "",
       "hash": ""
      },
      "# Based on http://trac.webkit.org/browser/trunk/LayoutTests/fast/url/script-tests/relative.js",
      {
       "input": "http://www.google.com/foo?bar=baz#",
       "base": "about:blank",
       "href": "http://www.google.com/foo?bar=baz#",
       "origin": "http://www.google.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www.google.com",
       "hostname": "www.google.com",
       "port": "",
       "pathname": "/foo",
       "search": "?bar=baz",
       "hash": ""
      },
      {
       "input": "http://www.google.com/foo?bar=baz# »",
       "base": "about:blank",
       "href": "http://www.google.com/foo?bar=baz#%20%C2%BB",
       "origin": "http://www.google.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www.google.com",
       "hostname": "www.google.com",
       "port": "",
       "pathname": "/foo",
       "search": "?bar=baz",
       "hash": "#%20%C2%BB"
      },
      {
       "input": "data:test# »",
       "base": "about:blank",
       "href": "data:test#%20%C2%BB",
       "origin": "null",
       "protocol": "data:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "test",
       "search": "",
       "hash": "#%20%C2%BB"
      },
      {
       "input": "http://www.google.com",
       "base": "about:blank",
       "href": "http://www.google.com/",
       "origin": "http://www.google.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www.google.com",
       "hostname": "www.google.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://192.0x00A80001",
       "base": "about:blank",
       "href": "http://192.168.0.1/",
       "origin": "http://192.168.0.1",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "192.168.0.1",
       "hostname": "192.168.0.1",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://www/foo%2Ehtml",
       "base": "about:blank",
       "href": "http://www/foo%2Ehtml",
       "origin": "http://www",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www",
       "hostname": "www",
       "port": "",
       "pathname": "/foo%2Ehtml",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://www/foo/%2E/html",
       "base": "about:blank",
       "href": "http://www/foo/html",
       "origin": "http://www",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www",
       "hostname": "www",
       "port": "",
       "pathname": "/foo/html",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://user:pass@/",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http://%25DOMAIN:foobar@foodomain.com/",
       "base": "about:blank",
       "href": "http://%25DOMAIN:foobar@foodomain.com/",
       "origin": "http://foodomain.com",
       "protocol": "http:",
       "username": "%25DOMAIN",
       "password": "foobar",
       "host": "foodomain.com",
       "hostname": "foodomain.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:\\\\www.google.com\\foo",
       "base": "about:blank",
       "href": "http://www.google.com/foo",
       "origin": "http://www.google.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www.google.com",
       "hostname": "www.google.com",
       "port": "",
       "pathname": "/foo",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://foo:80/",
       "base": "about:blank",
       "href": "http://foo/",
       "origin": "http://foo",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "foo",
       "hostname": "foo",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://foo:81/",
       "base": "about:blank",
       "href": "http://foo:81/",
       "origin": "http://foo:81",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "foo:81",
       "hostname": "foo",
       "port": "81",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "httpa://foo:80/",
       "base": "about:blank",
       "href": "httpa://foo:80/",
       "origin": "null",
       "protocol": "httpa:",
       "username": "",
       "password": "",
       "host": "foo:80",
       "hostname": "foo",
       "port": "80",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://foo:-80/",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "https://foo:443/",
       "base": "about:blank",
       "href": "https://foo/",
       "origin": "https://foo",
       "protocol": "https:",
       "username": "",
       "password": "",
       "host": "foo",
       "hostname": "foo",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "https://foo:80/",
       "base": "about:blank",
       "href": "https://foo:80/",
       "origin": "https://foo:80",
       "protocol": "https:",
       "username": "",
       "password": "",
       "host": "foo:80",
       "hostname": "foo",
       "port": "80",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ftp://foo:21/",
       "base": "about:blank",
       "href": "ftp://foo/",
       "origin": "ftp://foo",
       "protocol": "ftp:",
       "username": "",
       "password": "",
       "host": "foo",
       "hostname": "foo",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ftp://foo:80/",
       "base": "about:blank",
       "href": "ftp://foo:80/",
       "origin": "ftp://foo:80",
       "protocol": "ftp:",
       "username": "",
       "password": "",
       "host": "foo:80",
       "hostname": "foo",
       "port": "80",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "gopher://foo:70/",
       "base": "about:blank",
       "href": "gopher://foo:70/",
       "protocol": "gopher:",
       "username": "",
       "password": "",
       "host": "foo:70",
       "hostname": "foo",
       "port": "70",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "gopher://foo:443/",
       "base": "about:blank",
       "href": "gopher://foo:443/",
       "protocol": "gopher:",
       "username": "",
       "password": "",
       "host": "foo:443",
       "hostname": "foo",
       "port": "443",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ws://foo:80/",
       "base": "about:blank",
       "href": "ws://foo/",
       "origin": "ws://foo",
       "protocol": "ws:",
       "username": "",
       "password": "",
       "host": "foo",
       "hostname": "foo",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ws://foo:81/",
       "base": "about:blank",
       "href": "ws://foo:81/",
       "origin": "ws://foo:81",
       "protocol": "ws:",
       "username": "",
       "password": "",
       "host": "foo:81",
       "hostname": "foo",
       "port": "81",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ws://foo:443/",
       "base": "about:blank",
       "href": "ws://foo:443/",
       "origin": "ws://foo:443",
       "protocol": "ws:",
       "username": "",
       "password": "",
       "host": "foo:443",
       "hostname": "foo",
       "port": "443",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ws://foo:815/",
       "base": "about:blank",
       "href": "ws://foo:815/",
       "origin": "ws://foo:815",
       "protocol": "ws:",
       "username": "",
       "password": "",
       "host": "foo:815",
       "hostname": "foo",
       "port": "815",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "wss://foo:80/",
       "base": "about:blank",
       "href": "wss://foo:80/",
       "origin": "wss://foo:80",
       "protocol": "wss:",
       "username": "",
       "password": "",
       "host": "foo:80",
       "hostname": "foo",
       "port": "80",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "wss://foo:81/",
       "base": "about:blank",
       "href": "wss://foo:81/",
       "origin": "wss://foo:81",
       "protocol": "wss:",
       "username": "",
       "password": "",
       "host": "foo:81",
       "hostname": "foo",
       "port": "81",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "wss://foo:443/",
       "base": "about:blank",
       "href": "wss://foo/",
       "origin": "wss://foo",
       "protocol": "wss:",
       "username": "",
       "password": "",
       "host": "foo",
       "hostname": "foo",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "wss://foo:815/",
       "base": "about:blank",
       "href": "wss://foo:815/",
       "origin": "wss://foo:815",
       "protocol": "wss:",
       "username": "",
       "password": "",
       "host": "foo:815",
       "hostname": "foo",
       "port": "815",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:/example.com/",
       "base": "about:blank",
       "href": "http://example.com/",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ftp:/example.com/",
       "base": "about:blank",
       "href": "ftp://example.com/",
       "origin": "ftp://example.com",
       "protocol": "ftp:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "https:/example.com/",
       "base": "about:blank",
       "href": "https://example.com/",
       "origin": "https://example.com",
       "protocol": "https:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "madeupscheme:/example.com/",
       "base": "about:blank",
       "href": "madeupscheme:/example.com/",
       "origin": "null",
       "protocol": "madeupscheme:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "file:/example.com/",
       "base": "about:blank",
       "href": "file:///example.com/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ftps:/example.com/",
       "base": "about:blank",
       "href": "ftps:/example.com/",
       "origin": "null",
       "protocol": "ftps:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "gopher:/example.com/",
       "base": "about:blank",
       "href": "gopher:/example.com/",
       "protocol": "gopher:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ws:/example.com/",
       "base": "about:blank",
       "href": "ws://example.com/",
       "origin": "ws://example.com",
       "protocol": "ws:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "wss:/example.com/",
       "base": "about:blank",
       "href": "wss://example.com/",
       "origin": "wss://example.com",
       "protocol": "wss:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "data:/example.com/",
       "base": "about:blank",
       "href": "data:/example.com/",
       "origin": "null",
       "protocol": "data:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "javascript:/example.com/",
       "base": "about:blank",
       "href": "javascript:/example.com/",
       "origin": "null",
       "protocol": "javascript:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "mailto:/example.com/",
       "base": "about:blank",
       "href": "mailto:/example.com/",
       "origin": "null",
       "protocol": "mailto:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:example.com/",
       "base": "about:blank",
       "href": "http://example.com/",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ftp:example.com/",
       "base": "about:blank",
       "href": "ftp://example.com/",
       "origin": "ftp://example.com",
       "protocol": "ftp:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "https:example.com/",
       "base": "about:blank",
       "href": "https://example.com/",
       "origin": "https://example.com",
       "protocol": "https:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "madeupscheme:example.com/",
       "base": "about:blank",
       "href": "madeupscheme:example.com/",
       "origin": "null",
       "protocol": "madeupscheme:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ftps:example.com/",
       "base": "about:blank",
       "href": "ftps:example.com/",
       "origin": "null",
       "protocol": "ftps:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "gopher:example.com/",
       "base": "about:blank",
       "href": "gopher:example.com/",
       "protocol": "gopher:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ws:example.com/",
       "base": "about:blank",
       "href": "ws://example.com/",
       "origin": "ws://example.com",
       "protocol": "ws:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "wss:example.com/",
       "base": "about:blank",
       "href": "wss://example.com/",
       "origin": "wss://example.com",
       "protocol": "wss:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "data:example.com/",
       "base": "about:blank",
       "href": "data:example.com/",
       "origin": "null",
       "protocol": "data:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "javascript:example.com/",
       "base": "about:blank",
       "href": "javascript:example.com/",
       "origin": "null",
       "protocol": "javascript:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "example.com/",
       "search": "",
       "hash": ""
      },
      {
       "input": "mailto:example.com/",
       "base": "about:blank",
       "href": "mailto:example.com/",
       "origin": "null",
       "protocol": "mailto:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "example.com/",
       "search": "",
       "hash": ""
      },
      "# Based on http://trac.webkit.org/browser/trunk/LayoutTests/fast/url/segments-userinfo-vs-host.html",
      {
       "input": "http:@www.example.com",
       "base": "about:blank",
       "href": "http://www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:/@www.example.com",
       "base": "about:blank",
       "href": "http://www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://@www.example.com",
       "base": "about:blank",
       "href": "http://www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:a:b@www.example.com",
       "base": "about:blank",
       "href": "http://a:b@www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "a",
       "password": "b",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:/a:b@www.example.com",
       "base": "about:blank",
       "href": "http://a:b@www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "a",
       "password": "b",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://a:b@www.example.com",
       "base": "about:blank",
       "href": "http://a:b@www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "a",
       "password": "b",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://@pple.com",
       "base": "about:blank",
       "href": "http://pple.com/",
       "origin": "http://pple.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "pple.com",
       "hostname": "pple.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http::b@www.example.com",
       "base": "about:blank",
       "href": "http://:b@www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "",
       "password": "b",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:/:b@www.example.com",
       "base": "about:blank",
       "href": "http://:b@www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "",
       "password": "b",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://:b@www.example.com",
       "base": "about:blank",
       "href": "http://:b@www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "",
       "password": "b",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:/:@/www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http://user@/www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http:@/www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http:/@/www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http://@/www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "https:@/www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http:a:b@/www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http:/a:b@/www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http://a:b@/www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http::@/www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http:a:@www.example.com",
       "base": "about:blank",
       "href": "http://a@www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "a",
       "password": "",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:/a:@www.example.com",
       "base": "about:blank",
       "href": "http://a@www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "a",
       "password": "",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://a:@www.example.com",
       "base": "about:blank",
       "href": "http://a@www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "a",
       "password": "",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://www.@pple.com",
       "base": "about:blank",
       "href": "http://www.@pple.com/",
       "origin": "http://pple.com",
       "protocol": "http:",
       "username": "www.",
       "password": "",
       "host": "pple.com",
       "hostname": "pple.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http:@:www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http:/@:www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http://@:www.example.com",
       "base": "about:blank",
       "failure": true
      },
      {
       "input": "http://:@www.example.com",
       "base": "about:blank",
       "href": "http://www.example.com/",
       "origin": "http://www.example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www.example.com",
       "hostname": "www.example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "# Others",
      {
        "input": "/",
        "base": "http://www.example.com/test",
        "href": "http://www.example.com/",
        "origin": "http://www.example.com",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "www.example.com",
        "hostname": "www.example.com",
        "port": "",
        "pathname": "/",
        "search": "",
        "hash": ""
      },
      {
        "input": "/test.txt",
        "base": "http://www.example.com/test",
        "href": "http://www.example.com/test.txt",
        "origin": "http://www.example.com",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "www.example.com",
        "hostname": "www.example.com",
        "port": "",
        "pathname": "/test.txt",
        "search": "",
        "hash": ""
      },
      {
        "input": ".",
        "base": "http://www.example.com/test",
        "href": "http://www.example.com/",
        "origin": "http://www.example.com",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "www.example.com",
        "hostname": "www.example.com",
        "port": "",
        "pathname": "/",
        "search": "",
        "hash": ""
      },
      {
        "input": "..",
        "base": "http://www.example.com/test",
        "href": "http://www.example.com/",
        "origin": "http://www.example.com",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "www.example.com",
        "hostname": "www.example.com",
        "port": "",
        "pathname": "/",
        "search": "",
        "hash": ""
      },
      {
        "input": "test.txt",
        "base": "http://www.example.com/test",
        "href": "http://www.example.com/test.txt",
        "origin": "http://www.example.com",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "www.example.com",
        "hostname": "www.example.com",
        "port": "",
        "pathname": "/test.txt",
        "search": "",
        "hash": ""
      },
      {
        "input": "./test.txt",
        "base": "http://www.example.com/test",
        "href": "http://www.example.com/test.txt",
        "origin": "http://www.example.com",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "www.example.com",
        "hostname": "www.example.com",
        "port": "",
        "pathname": "/test.txt",
        "search": "",
        "hash": ""
      },
      {
        "input": "../test.txt",
        "base": "http://www.example.com/test",
        "href": "http://www.example.com/test.txt",
        "origin": "http://www.example.com",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "www.example.com",
        "hostname": "www.example.com",
        "port": "",
        "pathname": "/test.txt",
        "search": "",
        "hash": ""
      },
      {
        "input": "../aaa/test.txt",
        "base": "http://www.example.com/test",
        "href": "http://www.example.com/aaa/test.txt",
        "origin": "http://www.example.com",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "www.example.com",
        "hostname": "www.example.com",
        "port": "",
        "pathname": "/aaa/test.txt",
        "search": "",
        "hash": ""
      },
      {
        "input": "../../test.txt",
        "base": "http://www.example.com/test",
        "href": "http://www.example.com/test.txt",
        "origin": "http://www.example.com",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "www.example.com",
        "hostname": "www.example.com",
        "port": "",
        "pathname": "/test.txt",
        "search": "",
        "hash": ""
      },
      {
        "input": "中/test.txt",
        "base": "http://www.example.com/test",
        "href": "http://www.example.com/%E4%B8%AD/test.txt",
        "origin": "http://www.example.com",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "www.example.com",
        "hostname": "www.example.com",
        "port": "",
        "pathname": "/%E4%B8%AD/test.txt",
        "search": "",
        "hash": ""
      },
      {
       "input": "http://www.example2.com",
       "base": "http://www.example.com/test",
       "href": "http://www.example2.com/",
       "origin": "http://www.example2.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www.example2.com",
       "hostname": "www.example2.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "//www.example2.com",
       "base": "http://www.example.com/test",
       "href": "http://www.example2.com/",
       "origin": "http://www.example2.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www.example2.com",
       "hostname": "www.example2.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "file:...",
       "base": "http://www.example.com/test",
       "href": "file:///...",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/...",
       "search": "",
       "hash": ""
      },
      {
       "input": "file:..",
       "base": "http://www.example.com/test",
       "href": "file:///",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "file:a",
       "base": "http://www.example.com/test",
       "href": "file:///a",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/a",
       "search": "",
       "hash": ""
      },
      "# Based on http://trac.webkit.org/browser/trunk/LayoutTests/fast/url/host.html",
      "Basic canonicalization, uppercase should be converted to lowercase",
      {
       "input": "http://ExAmPlE.CoM",
       "base": "http://other.com/",
       "href": "http://example.com/",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
        "input": "http://example example.com",
        "base": "http://other.com/",
        "failure": true
      },
      {
        "input": "http://Goo%20 goo%7C|.com",
        "base": "http://other.com/",
        "failure": true
      },
      {
       "input": "http://[]",
       "base": "http://other.com/",
       "failure": true
      },
      {
       "input": "http://[:]",
       "base": "http://other.com/",
       "failure": true
      },
      "U+3000 is mapped to U+0020 (space) which is disallowed",
      {
        "input": "http://GOO\u00a0\u3000goo.com",
        "base": "http://other.com/",
        "failure": true
      },
      "Other types of space (no-break, zero-width, zero-width-no-break) are name-prepped away to nothing. U+200B, U+2060, and U+FEFF, are ignored",
      {
       "input": "http://GOO\u200b\u2060\ufeffgoo.com",
       "base": "http://other.com/",
       "href": "http://googoo.com/",
       "origin": "http://googoo.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "googoo.com",
       "hostname": "googoo.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "Leading and trailing C0 control or space",
      {
       "input": "\u0000\u001b\u0004\u0012 http://example.com/\u001f \u000d ",
       "base": "about:blank",
       "href": "http://example.com/",
       "origin": "http://example.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.com",
       "hostname": "example.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "Ideographic full stop (full-width period for Chinese, etc.) should be treated as a dot. U+3002 is mapped to U+002E (dot)",
      {
       "input": "http://www.foo。bar.com",
       "base": "http://other.com/",
       "href": "http://www.foo.bar.com/",
       "origin": "http://www.foo.bar.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "www.foo.bar.com",
       "hostname": "www.foo.bar.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "Invalid unicode characters should fail... U+FDD0 is disallowed; %ef%b7%90 is U+FDD0",
      {
        "input": "http://\ufdd0zyx.com",
        "base": "http://other.com/",
        "failure": true
      },
      "This is the same as previous but escaped",
      {
        "input": "http://%ef%b7%90zyx.com",
        "base": "http://other.com/",
        "failure": true
      },
      "U+FFFD",
      {
        "input": "https://\ufffd",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "https://%EF%BF%BD",
        "base": "about:blank",
        "failure": true
      },
      {
       "input": "https://x/\ufffd?\ufffd#\ufffd",
       "base": "about:blank",
       "href": "https://x/%EF%BF%BD?%EF%BF%BD#%EF%BF%BD",
       "origin": "https://x",
       "protocol": "https:",
       "username": "",
       "password": "",
       "host": "x",
       "hostname": "x",
       "port": "",
       "pathname": "/%EF%BF%BD",
       "search": "?%EF%BF%BD",
       "hash": "#%EF%BF%BD"
      },
      "Test name prepping, fullwidth input should be converted to ASCII and NOT IDN-ized. This is 'Go' in fullwidth UTF-8/UTF-16.",
      {
       "input": "http://Ｇｏ.com",
       "base": "http://other.com/",
       "href": "http://go.com/",
       "origin": "http://go.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "go.com",
       "hostname": "go.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "URL spec forbids the following. https://www.w3.org/Bugs/Public/show_bug.cgi?id=24257",
      {
        "input": "http://％４１.com",
        "base": "http://other.com/",
        "failure": true
      },
      {
        "input": "http://%ef%bc%85%ef%bc%94%ef%bc%91.com",
        "base": "http://other.com/",
        "failure": true
      },
      "...%00 in fullwidth should fail (also as escaped UTF-8 input)",
      {
        "input": "http://％００.com",
        "base": "http://other.com/",
        "failure": true
      },
      {
        "input": "http://%ef%bc%85%ef%bc%90%ef%bc%90.com",
        "base": "http://other.com/",
        "failure": true
      },
      "Basic IDN support, UTF-8 and UTF-16 input should be converted to IDN",
      {
       "input": "http://你好你好",
       "base": "http://other.com/",
       "href": "http://xn--6qqa088eba/",
       "origin": "http://xn--6qqa088eba",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "xn--6qqa088eba",
       "hostname": "xn--6qqa088eba",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "https://faß.ExAmPlE/",
       "base": "about:blank",
       "href": "https://xn--fa-hia.example/",
       "origin": "https://xn--fa-hia.example",
       "protocol": "https:",
       "username": "",
       "password": "",
       "host": "xn--fa-hia.example",
       "hostname": "xn--fa-hia.example",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "sc://faß.ExAmPlE/",
       "base": "about:blank",
       "href": "sc://fa%C3%9F.ExAmPlE/",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "fa%C3%9F.ExAmPlE",
       "hostname": "fa%C3%9F.ExAmPlE",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "Invalid escaped characters should fail and the percents should be escaped. https://www.w3.org/Bugs/Public/show_bug.cgi?id=24191",
      {
        "input": "http://%zz%66%a.com",
        "base": "http://other.com/",
        "failure": true
      },
      "If we get an invalid character that has been escaped.",
      {
        "input": "http://%25",
        "base": "http://other.com/",
        "failure": true
      },
      {
        "input": "http://hello%00",
        "base": "http://other.com/",
        "failure": true
      },
      "Escaped numbers should be treated like IP addresses if they are.",
      {
       "input": "http://%30%78%63%30%2e%30%32%35%30.01",
       "base": "http://other.com/",
       "href": "http://192.168.0.1/",
       "origin": "http://192.168.0.1",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "192.168.0.1",
       "hostname": "192.168.0.1",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://%30%78%63%30%2e%30%32%35%30.01%2e",
       "base": "http://other.com/",
       "href": "http://192.168.0.1/",
       "origin": "http://192.168.0.1",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "192.168.0.1",
       "hostname": "192.168.0.1",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://192.168.0.257",
       "base": "http://other.com/",
       "failure": true
      },
      "Invalid escaping in hosts causes failure",
      {
        "input": "http://%3g%78%63%30%2e%30%32%35%30%2E.01",
        "base": "http://other.com/",
        "failure": true
      },
      "A space in a host causes failure",
      {
        "input": "http://192.168.0.1 hello",
        "base": "http://other.com/",
        "failure": true
      },
      {
        "input": "https://x x:12",
        "base": "about:blank",
        "failure": true
      },
      "Fullwidth and escaped UTF-8 fullwidth should still be treated as IP",
      {
       "input": "http://０Ｘｃ０．０２５０．０１",
       "base": "http://other.com/",
       "href": "http://192.168.0.1/",
       "origin": "http://192.168.0.1",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "192.168.0.1",
       "hostname": "192.168.0.1",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "Domains with empty labels",
      {
       "input": "http://./",
       "base": "about:blank",
       "href": "http://./",
       "origin": "http://.",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": ".",
       "hostname": ".",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://../",
       "base": "about:blank",
       "href": "http://../",
       "origin": "http://..",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "..",
       "hostname": "..",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "Broken IPv6",
      {
        "input": "http://[www.google.com]/",
        "base": "about:blank",
        "failure": true
      },
      {
       "input": "http://[google.com]",
       "base": "http://other.com/",
       "failure": true
      },
      {
       "input": "http://[::1.2.3.4x]",
       "base": "http://other.com/",
       "failure": true
      },
      {
       "input": "http://[::1.2.3.]",
       "base": "http://other.com/",
       "failure": true
      },
      {
       "input": "http://[::1.2.]",
       "base": "http://other.com/",
       "failure": true
      },
      {
       "input": "http://[::1.]",
       "base": "http://other.com/",
       "failure": true
      },
      "Misc Unicode",
      {
        "input": "http://foo:💩@example.com/bar",
        "base": "http://other.com/",
        "href": "http://foo:%F0%9F%92%A9@example.com/bar",
        "origin": "http://example.com",
        "protocol": "http:",
        "username": "foo",
        "password": "%F0%9F%92%A9",
        "host": "example.com",
        "hostname": "example.com",
        "port": "",
        "pathname": "/bar",
        "search": "",
        "hash": ""
      },
      "# resolving a fragment against any scheme succeeds",
      {
       "input": "#",
       "base": "test:test",
       "href": "test:test#",
       "origin": "null",
       "protocol": "test:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "test",
       "search": "",
       "hash": ""
      },
      {
       "input": "#x",
       "base": "mailto:x@x.com",
       "href": "mailto:x@x.com#x",
       "origin": "null",
       "protocol": "mailto:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "x@x.com",
       "search": "",
       "hash": "#x"
      },
      {
       "input": "#x",
       "base": "data:,",
       "href": "data:,#x",
       "origin": "null",
       "protocol": "data:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": ",",
       "search": "",
       "hash": "#x"
      },
      {
       "input": "#x",
       "base": "about:blank",
       "href": "about:blank#x",
       "origin": "null",
       "protocol": "about:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "blank",
       "search": "",
       "hash": "#x"
      },
      {
       "input": "#",
       "base": "test:test?test",
       "href": "test:test?test#",
       "origin": "null",
       "protocol": "test:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "test",
       "search": "?test",
       "hash": ""
      },
      "# multiple @ in authority state",
      {
       "input": "https://@test@test@example:800/",
       "base": "http://doesnotmatter/",
       "href": "https://%40test%40test@example:800/",
       "origin": "https://example:800",
       "protocol": "https:",
       "username": "%40test%40test",
       "password": "",
       "host": "example:800",
       "hostname": "example",
       "port": "800",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "https://@@@example",
       "base": "http://doesnotmatter/",
       "href": "https://%40%40@example/",
       "origin": "https://example",
       "protocol": "https:",
       "username": "%40%40",
       "password": "",
       "host": "example",
       "hostname": "example",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "non-az-09 characters",
      {
       "input": "http://`{}:`{}@h/`{}?`{}",
       "base": "http://doesnotmatter/",
       "href": "http://%60%7B%7D:%60%7B%7D@h/%60%7B%7D?`{}",
       "origin": "http://h",
       "protocol": "http:",
       "username": "%60%7B%7D",
       "password": "%60%7B%7D",
       "host": "h",
       "hostname": "h",
       "port": "",
       "pathname": "/%60%7B%7D",
       "search": "?`{}",
       "hash": ""
      },
      "# Credentials in base",
      {
        "input": "/some/path",
        "base": "http://user@example.org/smth",
        "href": "http://user@example.org/some/path",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "user",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/some/path",
        "search": "",
        "hash": ""
      },
      {
        "input": "",
        "base": "http://user:pass@example.org:21/smth",
        "href": "http://user:pass@example.org:21/smth",
        "origin": "http://example.org:21",
        "protocol": "http:",
        "username": "user",
        "password": "pass",
        "host": "example.org:21",
        "hostname": "example.org",
        "port": "21",
        "pathname": "/smth",
        "search": "",
        "hash": ""
      },
      {
        "input": "/some/path",
        "base": "http://user:pass@example.org:21/smth",
        "href": "http://user:pass@example.org:21/some/path",
        "origin": "http://example.org:21",
        "protocol": "http:",
        "username": "user",
        "password": "pass",
        "host": "example.org:21",
        "hostname": "example.org",
        "port": "21",
        "pathname": "/some/path",
        "search": "",
        "hash": ""
      },
      "# a set of tests designed by zcorpan for relative URLs with unknown schemes",
      {
        "input": "i",
        "base": "sc:sd",
        "failure": true
      },
      {
        "input": "i",
        "base": "sc:sd/sd",
        "failure": true
      },
      {
       "input": "i",
       "base": "sc:/pa/pa",
       "href": "sc:/pa/i",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/pa/i",
       "search": "",
       "hash": ""
      },
      {
        "input": "i",
        "base": "sc://ho/pa",
        "href": "sc://ho/i",
        "origin": "null",
        "protocol": "sc:",
        "username": "",
        "password": "",
        "host": "ho",
        "hostname": "ho",
        "port": "",
        "pathname": "/i",
        "search": "",
        "hash": ""
      },
      {
        "input": "i",
        "base": "sc:///pa/pa",
        "href": "sc:///pa/i",
        "origin": "null",
        "protocol": "sc:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/pa/i",
        "search": "",
        "hash": ""
      },
      {
        "input": "../i",
        "base": "sc:sd",
        "failure": true
      },
      {
        "input": "../i",
        "base": "sc:sd/sd",
        "failure": true
      },
      {
       "input": "../i",
       "base": "sc:/pa/pa",
       "href": "sc:/i",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/i",
       "search": "",
       "hash": ""
      },
      {
       "input": "../i",
       "base": "sc://ho/pa",
       "href": "sc://ho/i",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "ho",
       "hostname": "ho",
       "port": "",
       "pathname": "/i",
       "search": "",
       "hash": ""
      },
      {
        "input": "../i",
        "base": "sc:///pa/pa",
        "href": "sc:///i",
        "origin": "null",
        "protocol": "sc:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/i",
        "search": "",
        "hash": ""
      },
      {
        "input": "/i",
        "base": "sc:sd",
        "failure": true
      },
      {
        "input": "/i",
        "base": "sc:sd/sd",
        "failure": true
      },
      {
       "input": "/i",
       "base": "sc:/pa/pa",
       "href": "sc:/i",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/i",
       "search": "",
       "hash": ""
      },
      {
        "input": "/i",
        "base": "sc://ho/pa",
        "href": "sc://ho/i",
        "origin": "null",
        "protocol": "sc:",
        "username": "",
        "password": "",
        "host": "ho",
        "hostname": "ho",
        "port": "",
        "pathname": "/i",
        "search": "",
        "hash": ""
      },
      {
        "input": "/i",
        "base": "sc:///pa/pa",
        "href": "sc:///i",
        "origin": "null",
        "protocol": "sc:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/i",
        "search": "",
        "hash": ""
      },
      {
        "input": "?i",
        "base": "sc:sd",
        "failure": true
      },
      {
        "input": "?i",
        "base": "sc:sd/sd",
        "failure": true
      },
      {
       "input": "?i",
       "base": "sc:/pa/pa",
       "href": "sc:/pa/pa?i",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/pa/pa",
       "search": "?i",
       "hash": ""
      },
      {
        "input": "?i",
        "base": "sc://ho/pa",
        "href": "sc://ho/pa?i",
        "origin": "null",
        "protocol": "sc:",
        "username": "",
        "password": "",
        "host": "ho",
        "hostname": "ho",
        "port": "",
        "pathname": "/pa",
        "search": "?i",
        "hash": ""
      },
      {
        "input": "?i",
        "base": "sc:///pa/pa",
        "href": "sc:///pa/pa?i",
        "origin": "null",
        "protocol": "sc:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/pa/pa",
        "search": "?i",
        "hash": ""
      },
      {
       "input": "#i",
       "base": "sc:sd",
       "href": "sc:sd#i",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "sd",
       "search": "",
       "hash": "#i"
      },
      {
       "input": "#i",
       "base": "sc:sd/sd",
       "href": "sc:sd/sd#i",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "sd/sd",
       "search": "",
       "hash": "#i"
      },
      {
       "input": "#i",
       "base": "sc:/pa/pa",
       "href": "sc:/pa/pa#i",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/pa/pa",
       "search": "",
       "hash": "#i"
      },
      {
        "input": "#i",
        "base": "sc://ho/pa",
        "href": "sc://ho/pa#i",
        "origin": "null",
        "protocol": "sc:",
        "username": "",
        "password": "",
        "host": "ho",
        "hostname": "ho",
        "port": "",
        "pathname": "/pa",
        "search": "",
        "hash": "#i"
      },
      {
        "input": "#i",
        "base": "sc:///pa/pa",
        "href": "sc:///pa/pa#i",
        "origin": "null",
        "protocol": "sc:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/pa/pa",
        "search": "",
        "hash": "#i"
      },
      "# make sure that relative URL logic works on known typically non-relative schemes too",
      {
       "input": "about:/../",
       "base": "about:blank",
       "href": "about:/",
       "origin": "null",
       "protocol": "about:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "data:/../",
       "base": "about:blank",
       "href": "data:/",
       "origin": "null",
       "protocol": "data:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "javascript:/../",
       "base": "about:blank",
       "href": "javascript:/",
       "origin": "null",
       "protocol": "javascript:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "mailto:/../",
       "base": "about:blank",
       "href": "mailto:/",
       "origin": "null",
       "protocol": "mailto:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "# unknown schemes and their hosts",
      {
       "input": "sc://ñ.test/",
       "base": "about:blank",
       "href": "sc://%C3%B1.test/",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "%C3%B1.test",
       "hostname": "%C3%B1.test",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
        "input": "sc://\u0000/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "sc:// /",
        "base": "about:blank",
        "failure": true
      },
      {
       "input": "sc://%/",
       "base": "about:blank",
       "href": "sc://%/",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "%",
       "hostname": "%",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
        "input": "sc://@/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "sc://te@s:t@/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "sc://:/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "sc://:12/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "sc://[/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "sc://\\/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "sc://]/",
        "base": "about:blank",
        "failure": true
      },
      {
       "input": "x",
       "base": "sc://ñ",
       "href": "sc://%C3%B1/x",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "%C3%B1",
       "hostname": "%C3%B1",
       "port": "",
       "pathname": "/x",
       "search": "",
       "hash": ""
      },
      "# unknown schemes and backslashes",
      {
       "input": "sc:\\../",
       "base": "about:blank",
       "href": "sc:\\../",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "\\../",
       "search": "",
       "hash": ""
      },
      "# unknown scheme with path looking like a password",
      {
       "input": "sc::a@example.net",
       "base": "about:blank",
       "href": "sc::a@example.net",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": ":a@example.net",
       "search": "",
       "hash": ""
      },
      "# unknown scheme with bogus percent-encoding",
      {
       "input": "wow:%NBD",
       "base": "about:blank",
       "href": "wow:%NBD",
       "origin": "null",
       "protocol": "wow:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "%NBD",
       "search": "",
       "hash": ""
      },
      {
       "input": "wow:%1G",
       "base": "about:blank",
       "href": "wow:%1G",
       "origin": "null",
       "protocol": "wow:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "%1G",
       "search": "",
       "hash": ""
      },
      "# Hosts and percent-encoding",
      {
        "input": "ftp://example.com%80/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "ftp://example.com%A0/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "https://example.com%80/",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "https://example.com%A0/",
        "base": "about:blank",
        "failure": true
      },
      {
       "input": "ftp://%e2%98%83",
       "base": "about:blank",
       "href": "ftp://xn--n3h/",
       "origin": "ftp://xn--n3h",
       "protocol": "ftp:",
       "username": "",
       "password": "",
       "host": "xn--n3h",
       "hostname": "xn--n3h",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "https://%e2%98%83",
       "base": "about:blank",
       "href": "https://xn--n3h/",
       "origin": "https://xn--n3h",
       "protocol": "https:",
       "username": "",
       "password": "",
       "host": "xn--n3h",
       "hostname": "xn--n3h",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "# tests from jsdom/whatwg-url designed for code coverage",
      {
       "input": "http://127.0.0.1:10100/relative_import.html",
       "base": "about:blank",
       "href": "http://127.0.0.1:10100/relative_import.html",
       "origin": "http://127.0.0.1:10100",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "127.0.0.1:10100",
       "hostname": "127.0.0.1",
       "port": "10100",
       "pathname": "/relative_import.html",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://facebook.com/?foo=%7B%22abc%22",
       "base": "about:blank",
       "href": "http://facebook.com/?foo=%7B%22abc%22",
       "origin": "http://facebook.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "facebook.com",
       "hostname": "facebook.com",
       "port": "",
       "pathname": "/",
       "search": "?foo=%7B%22abc%22",
       "hash": ""
      },
      {
       "input": "https://localhost:3000/jqueryui@1.2.3",
       "base": "about:blank",
       "href": "https://localhost:3000/jqueryui@1.2.3",
       "origin": "https://localhost:3000",
       "protocol": "https:",
       "username": "",
       "password": "",
       "host": "localhost:3000",
       "hostname": "localhost",
       "port": "3000",
       "pathname": "/jqueryui@1.2.3",
       "search": "",
       "hash": ""
      },
      "# tab/LF/CR",
      {
       "input": "h\tt\nt\rp://h\to\ns\rt:9\t0\n0\r0/p\ta\nt\rh?q\tu\ne\rry#f\tr\na\rg",
       "base": "about:blank",
       "href": "http://host:9000/path?query#frag",
       "origin": "http://host:9000",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "host:9000",
       "hostname": "host",
       "port": "9000",
       "pathname": "/path",
       "search": "?query",
       "hash": "#frag"
      },
      "# Stringification of URL.searchParams",
      {
        "input": "?a=b&c=d",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/foo/bar?a=b&c=d",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/foo/bar",
        "search": "?a=b&c=d",
        "searchParams": "a=b&c=d",
        "hash": ""
      },
      {
       "input": "??a=b&c=d",
       "base": "http://example.org/foo/bar",
       "href": "http://example.org/foo/bar??a=b&c=d",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/foo/bar",
       "search": "??a=b&c=d",
       "searchParams": "%3Fa=b&c=d",
       "hash": ""
      },
      "# Scheme only",
      {
        "input": "http:",
        "base": "http://example.org/foo/bar",
        "href": "http://example.org/foo/bar",
        "origin": "http://example.org",
        "protocol": "http:",
        "username": "",
        "password": "",
        "host": "example.org",
        "hostname": "example.org",
        "port": "",
        "pathname": "/foo/bar",
        "search": "",
        "searchParams": "",
        "hash": ""
      },
      {
       "input": "http:",
       "base": "https://example.org/foo/bar",
       "failure": true
      },
      {
       "input": "sc:",
       "base": "https://example.org/foo/bar",
       "href": "sc:",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "",
       "search": "",
       "searchParams": "",
       "hash": ""
      },
      "# Percent encoding of fragments",
      {
       "input": "http://foo.bar/baz?qux#foo\bbar",
       "base": "about:blank",
       "href": "http://foo.bar/baz?qux#foo%08bar",
       "origin": "http://foo.bar",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "foo.bar",
       "hostname": "foo.bar",
       "port": "",
       "pathname": "/baz",
       "search": "?qux",
       "searchParams": "qux=",
       "hash": "#foo%08bar"
      },
      "# IPv4 parsing (via https://github.com/nodejs/node/pull/10317)",
      {
       "input": "http://192.168.257",
       "base": "http://other.com/",
       "href": "http://192.168.1.1/",
       "origin": "http://192.168.1.1",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "192.168.1.1",
       "hostname": "192.168.1.1",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://192.168.257.com",
       "base": "http://other.com/",
       "href": "http://192.168.257.com/",
       "origin": "http://192.168.257.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "192.168.257.com",
       "hostname": "192.168.257.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://256",
       "base": "http://other.com/",
       "href": "http://0.0.1.0/",
       "origin": "http://0.0.1.0",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "0.0.1.0",
       "hostname": "0.0.1.0",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://256.com",
       "base": "http://other.com/",
       "href": "http://256.com/",
       "origin": "http://256.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "256.com",
       "hostname": "256.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://999999999",
       "base": "http://other.com/",
       "href": "http://59.154.201.255/",
       "origin": "http://59.154.201.255",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "59.154.201.255",
       "hostname": "59.154.201.255",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://999999999.com",
       "base": "http://other.com/",
       "href": "http://999999999.com/",
       "origin": "http://999999999.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "999999999.com",
       "hostname": "999999999.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://10000000000",
       "base": "http://other.com/",
       "failure": true
      },
      {
       "input": "http://10000000000.com",
       "base": "http://other.com/",
       "href": "http://10000000000.com/",
       "origin": "http://10000000000.com",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "10000000000.com",
       "hostname": "10000000000.com",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://4294967295",
       "base": "http://other.com/",
       "href": "http://255.255.255.255/",
       "origin": "http://255.255.255.255",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "255.255.255.255",
       "hostname": "255.255.255.255",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://4294967296",
       "base": "http://other.com/",
       "failure": true
      },
      {
       "input": "http://0xffffffff",
       "base": "http://other.com/",
       "href": "http://255.255.255.255/",
       "origin": "http://255.255.255.255",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "255.255.255.255",
       "hostname": "255.255.255.255",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://0xffffffff1",
       "base": "http://other.com/",
       "failure": true
      },
      {
       "input": "http://256.256.256.256",
       "base": "http://other.com/",
       "failure": true
      },
      {
       "input": "https://0x.0x.0",
       "base": "about:blank",
       "href": "https://0.0.0.0/",
       "origin": "https://0.0.0.0",
       "protocol": "https:",
       "username": "",
       "password": "",
       "host": "0.0.0.0",
       "hostname": "0.0.0.0",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "More IPv4 parsing (via https://github.com/jsdom/whatwg-url/issues/92)",
      {
        "input": "https://0x100000000/test",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "https://256.0.0.1/test",
        "base": "about:blank",
        "failure": true
      },
      "# file URLs containing percent-encoded Windows drive letters (shouldn't work)",
      {
       "input": "file:///C%3A/",
       "base": "about:blank",
       "href": "file:///C%3A/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/C%3A/",
       "search": "",
       "hash": ""
      },
      {
       "input": "file:///C%7C/",
       "base": "about:blank",
       "href": "file:///C%7C/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/C%7C/",
       "search": "",
       "hash": ""
      },
      "# file URLs relative to other file URLs (via https://github.com/jsdom/whatwg-url/pull/60)",
      {
       "input": "pix/submit.gif",
       "base": "file:///C:/Users/Domenic/Dropbox/GitHub/tmpvar/jsdom/test/level2/html/files/anchor.html",
       "href": "file:///C:/Users/Domenic/Dropbox/GitHub/tmpvar/jsdom/test/level2/html/files/pix/submit.gif",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/C:/Users/Domenic/Dropbox/GitHub/tmpvar/jsdom/test/level2/html/files/pix/submit.gif",
       "search": "",
       "hash": ""
      },
      {
       "input": "..",
       "base": "file:///C:/",
       "href": "file:///C:/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/C:/",
       "search": "",
       "hash": ""
      },
      {
        "input": "..",
        "base": "file:///",
        "href": "file:///",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/",
        "search": "",
        "hash": ""
      },
      "# More file URL tests by zcorpan and annevk",
      {
       "input": "/",
       "base": "file:///C:/a/b",
       "href": "file:///C:/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/C:/",
       "search": "",
       "hash": ""
      },
      {
       "input": "//d:",
       "base": "file:///C:/a/b",
       "href": "file:///d:",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/d:",
       "search": "",
       "hash": ""
      },
      {
       "input": "//d:/..",
       "base": "file:///C:/a/b",
       "href": "file:///d:/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/d:/",
       "search": "",
       "hash": ""
      },
      {
        "input": "..",
        "base": "file:///ab:/",
        "href": "file:///",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/",
        "search": "",
        "hash": ""
      },
      {
        "input": "..",
        "base": "file:///1:/",
        "href": "file:///",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/",
        "search": "",
        "hash": ""
      },
      {
       "input": "",
       "base": "file:///test?test#test",
       "href": "file:///test?test",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/test",
       "search": "?test",
       "hash": ""
      },
      {
        "input": "file:",
        "base": "file:///test?test#test",
        "href": "file:///test?test",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/test",
        "search": "?test",
        "hash": ""
      },
      {
        "input": "?x",
        "base": "file:///test?test#test",
        "href": "file:///test?x",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/test",
        "search": "?x",
        "hash": ""
      },
      {
        "input": "file:?x",
        "base": "file:///test?test#test",
        "href": "file:///test?x",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/test",
        "search": "?x",
        "hash": ""
      },
      {
        "input": "#x",
        "base": "file:///test?test#test",
        "href": "file:///test?test#x",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/test",
        "search": "?test",
        "hash": "#x"
      },
      {
        "input": "file:#x",
        "base": "file:///test?test#test",
        "href": "file:///test?test#x",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/test",
        "search": "?test",
        "hash": "#x"
      },
      "# File URLs and many (back)slashes",
      {
       "input": "file:\\\\//",
       "base": "about:blank",
       "href": "file:////",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "//",
       "search": "",
       "hash": ""
      },
      {
       "input": "file:\\\\\\\\",
       "base": "about:blank",
       "href": "file:////",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "//",
       "search": "",
       "hash": ""
      },
      {
       "input": "file:\\\\\\\\?fox",
       "base": "about:blank",
       "href": "file:////?fox",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "//",
       "search": "?fox",
       "hash": ""
      },
      {
       "input": "file:\\\\\\\\#guppy",
       "base": "about:blank",
       "href": "file:////#guppy",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "//",
       "search": "",
       "hash": "#guppy"
      },
      {
       "input": "file://spider///",
       "base": "about:blank",
       "href": "file://spider///",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "spider",
       "hostname": "spider",
       "port": "",
       "pathname": "///",
       "search": "",
       "hash": ""
      },
      {
       "input": "file:\\\\localhost//",
       "base": "about:blank",
       "href": "file:////",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "//",
       "search": "",
       "hash": ""
      },
      {
       "input": "file:///localhost//cat",
       "base": "about:blank",
       "href": "file:///localhost//cat",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/localhost//cat",
       "search": "",
       "hash": ""
      },
      {
       "input": "file://\\/localhost//cat",
       "base": "about:blank",
       "href": "file:////localhost//cat",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "//localhost//cat",
       "search": "",
       "hash": ""
      },
      {
       "input": "file://localhost//a//../..//",
       "base": "about:blank",
       "href": "file://///",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "///",
       "search": "",
       "hash": ""
      },
      {
        "input": "/////mouse",
        "base": "file:///elephant",
        "href": "file://///mouse",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "///mouse",
        "search": "",
        "hash": ""
      },
      {
       "input": "\\//pig",
       "base": "file://lion/",
       "href": "file:///pig",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/pig",
       "search": "",
       "hash": ""
      },
      {
       "input": "\\/localhost//pig",
       "base": "file://lion/",
       "href": "file:////pig",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "//pig",
       "search": "",
       "hash": ""
      },
      {
       "input": "//localhost//pig",
       "base": "file://lion/",
       "href": "file:////pig",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "//pig",
       "search": "",
       "hash": ""
      },
      {
       "input": "/..//localhost//pig",
       "base": "file://lion/",
       "href": "file://lion//localhost//pig",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "lion",
       "hostname": "lion",
       "port": "",
       "pathname": "//localhost//pig",
       "search": "",
       "hash": ""
      },
      {
       "input": "file://",
       "base": "file://ape/",
       "href": "file:///",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "# File URLs with non-empty hosts",
      {
        "input": "/rooibos",
        "base": "file://tea/",
        "href": "file://tea/rooibos",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "tea",
        "hostname": "tea",
        "port": "",
        "pathname": "/rooibos",
        "search": "",
        "hash": ""
      },
      {
        "input": "/?chai",
        "base": "file://tea/",
        "href": "file://tea/?chai",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "tea",
        "hostname": "tea",
        "port": "",
        "pathname": "/",
        "search": "?chai",
        "hash": ""
      },
      "# Windows drive letter handling with the 'file:' base URL",
      {
       "input": "C|",
       "base": "file://host/dir/file",
       "href": "file://host/C:",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "host",
       "hostname": "host",
       "port": "",
       "pathname": "/C:",
       "search": "",
       "hash": ""
      },
      {
       "input": "C|#",
       "base": "file://host/dir/file",
       "href": "file://host/C:#",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "host",
       "hostname": "host",
       "port": "",
       "pathname": "/C:",
       "search": "",
       "hash": ""
      },
      {
       "input": "C|?",
       "base": "file://host/dir/file",
       "href": "file://host/C:?",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "host",
       "hostname": "host",
       "port": "",
       "pathname": "/C:",
       "search": "",
       "hash": ""
      },
      {
       "input": "C|/",
       "base": "file://host/dir/file",
       "href": "file://host/C:/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "host",
       "hostname": "host",
       "port": "",
       "pathname": "/C:/",
       "search": "",
       "hash": ""
      },
      {
       "input": "C|\n/",
       "base": "file://host/dir/file",
       "href": "file://host/C:/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "host",
       "hostname": "host",
       "port": "",
       "pathname": "/C:/",
       "search": "",
       "hash": ""
      },
      {
       "input": "C|\\",
       "base": "file://host/dir/file",
       "href": "file://host/C:/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "host",
       "hostname": "host",
       "port": "",
       "pathname": "/C:/",
       "search": "",
       "hash": ""
      },
      {
        "input": "C",
        "base": "file://host/dir/file",
        "href": "file://host/dir/C",
        "protocol": "file:",
        "username": "",
        "password": "",
        "host": "host",
        "hostname": "host",
        "port": "",
        "pathname": "/dir/C",
        "search": "",
        "hash": ""
      },
      {
       "input": "C|a",
       "base": "file://host/dir/file",
       "href": "file://host/dir/C|a",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "host",
       "hostname": "host",
       "port": "",
       "pathname": "/dir/C|a",
       "search": "",
       "hash": ""
      },
      "# Windows drive letter quirk with not empty host",
      {
       "input": "file://example.net/C:/",
       "base": "about:blank",
       "href": "file://example.net/C:/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "example.net",
       "hostname": "example.net",
       "port": "",
       "pathname": "/C:/",
       "search": "",
       "hash": ""
      },
      {
       "input": "file://1.2.3.4/C:/",
       "base": "about:blank",
       "href": "file://1.2.3.4/C:/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "1.2.3.4",
       "hostname": "1.2.3.4",
       "port": "",
       "pathname": "/C:/",
       "search": "",
       "hash": ""
      },
      {
       "input": "file://[1::8]/C:/",
       "base": "about:blank",
       "href": "file://[1::8]/C:/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "[1::8]",
       "hostname": "[1::8]",
       "port": "",
       "pathname": "/C:/",
       "search": "",
       "hash": ""
      },
      "# Windows drive letter quirk (no host)",
      {
       "input": "file:/C|/",
       "base": "about:blank",
       "href": "file:///C:/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/C:/",
       "search": "",
       "hash": ""
      },
      {
       "input": "file://C|/",
       "base": "about:blank",
       "href": "file:///C:/",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/C:/",
       "search": "",
       "hash": ""
      },
      "# file URLs without base URL by Rimas Misevičius",
      {
       "input": "file:",
       "base": "about:blank",
       "href": "file:///",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "file:?q=v",
       "base": "about:blank",
       "href": "file:///?q=v",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "?q=v",
       "hash": ""
      },
      {
       "input": "file:#frag",
       "base": "about:blank",
       "href": "file:///#frag",
       "protocol": "file:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": "#frag"
      },
      "# IPv6 tests",
      {
       "input": "http://[1:0::]",
       "base": "http://example.net/",
       "href": "http://[1::]/",
       "origin": "http://[1::]",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "[1::]",
       "hostname": "[1::]",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://[0:1:2:3:4:5:6:7:8]",
       "base": "http://example.net/",
       "failure": true
      },
      {
        "input": "https://[0::0::0]",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "https://[0:.0]",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "https://[0:0:]",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "https://[0:1:2:3:4:5:6:7.0.0.0.1]",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "https://[0:1.00.0.0.0]",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "https://[0:1.290.0.0.0]",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "https://[0:1.23.23]",
        "base": "about:blank",
        "failure": true
      },
      "# Empty host",
      {
        "input": "http://?",
        "base": "about:blank",
        "failure": true
      },
      {
        "input": "http://#",
        "base": "about:blank",
        "failure": true
      },
      "# Non-special-URL path tests",
      {
       "input": "sc://ñ",
       "base": "about:blank",
       "href": "sc://%C3%B1",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "%C3%B1",
       "hostname": "%C3%B1",
       "port": "",
       "pathname": "",
       "search": "",
       "hash": ""
      },
      {
       "input": "sc://ñ?x",
       "base": "about:blank",
       "href": "sc://%C3%B1?x",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "%C3%B1",
       "hostname": "%C3%B1",
       "port": "",
       "pathname": "",
       "search": "?x",
       "hash": ""
      },
      {
       "input": "sc://ñ#x",
       "base": "about:blank",
       "href": "sc://%C3%B1#x",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "%C3%B1",
       "hostname": "%C3%B1",
       "port": "",
       "pathname": "",
       "search": "",
       "hash": "#x"
      },
      {
       "input": "#x",
       "base": "sc://ñ",
       "href": "sc://%C3%B1#x",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "%C3%B1",
       "hostname": "%C3%B1",
       "port": "",
       "pathname": "",
       "search": "",
       "hash": "#x"
      },
      {
       "input": "?x",
       "base": "sc://ñ",
       "href": "sc://%C3%B1?x",
       "origin": "null",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "%C3%B1",
       "hostname": "%C3%B1",
       "port": "",
       "pathname": "",
       "search": "?x",
       "hash": ""
      },
      {
       "input": "sc://?",
       "base": "about:blank",
       "href": "sc://?",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "",
       "search": "",
       "hash": ""
      },
      {
       "input": "sc://#",
       "base": "about:blank",
       "href": "sc://#",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "",
       "search": "",
       "hash": ""
      },
      {
        "input": "///",
        "base": "sc://x/",
        "href": "sc:///",
        "protocol": "sc:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/",
        "search": "",
        "hash": ""
      },
      {
       "input": "////",
       "base": "sc://x/",
       "href": "sc:////",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "//",
       "search": "",
       "hash": ""
      },
      {
       "input": "////x/",
       "base": "sc://x/",
       "href": "sc:////x/",
       "protocol": "sc:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "//x/",
       "search": "",
       "hash": ""
      },
      {
       "input": "tftp://foobar.com/someconfig;mode=netascii",
       "base": "about:blank",
       "href": "tftp://foobar.com/someconfig;mode=netascii",
       "origin": "null",
       "protocol": "tftp:",
       "username": "",
       "password": "",
       "host": "foobar.com",
       "hostname": "foobar.com",
       "port": "",
       "pathname": "/someconfig;mode=netascii",
       "search": "",
       "hash": ""
      },
      {
       "input": "telnet://user:pass@foobar.com:23/",
       "base": "about:blank",
       "href": "telnet://user:pass@foobar.com:23/",
       "origin": "null",
       "protocol": "telnet:",
       "username": "user",
       "password": "pass",
       "host": "foobar.com:23",
       "hostname": "foobar.com",
       "port": "23",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "ut2004://10.10.10.10:7777/Index.ut2",
       "base": "about:blank",
       "href": "ut2004://10.10.10.10:7777/Index.ut2",
       "origin": "null",
       "protocol": "ut2004:",
       "username": "",
       "password": "",
       "host": "10.10.10.10:7777",
       "hostname": "10.10.10.10",
       "port": "7777",
       "pathname": "/Index.ut2",
       "search": "",
       "hash": ""
      },
      {
       "input": "redis://foo:bar@somehost:6379/0?baz=bam&qux=baz",
       "base": "about:blank",
       "href": "redis://foo:bar@somehost:6379/0?baz=bam&qux=baz",
       "origin": "null",
       "protocol": "redis:",
       "username": "foo",
       "password": "bar",
       "host": "somehost:6379",
       "hostname": "somehost",
       "port": "6379",
       "pathname": "/0",
       "search": "?baz=bam&qux=baz",
       "hash": ""
      },
      {
       "input": "rsync://foo@host:911/sup",
       "base": "about:blank",
       "href": "rsync://foo@host:911/sup",
       "origin": "null",
       "protocol": "rsync:",
       "username": "foo",
       "password": "",
       "host": "host:911",
       "hostname": "host",
       "port": "911",
       "pathname": "/sup",
       "search": "",
       "hash": ""
      },
      {
       "input": "git://github.com/foo/bar.git",
       "base": "about:blank",
       "href": "git://github.com/foo/bar.git",
       "origin": "null",
       "protocol": "git:",
       "username": "",
       "password": "",
       "host": "github.com",
       "hostname": "github.com",
       "port": "",
       "pathname": "/foo/bar.git",
       "search": "",
       "hash": ""
      },
      {
       "input": "irc://myserver.com:6999/channel?passwd",
       "base": "about:blank",
       "href": "irc://myserver.com:6999/channel?passwd",
       "origin": "null",
       "protocol": "irc:",
       "username": "",
       "password": "",
       "host": "myserver.com:6999",
       "hostname": "myserver.com",
       "port": "6999",
       "pathname": "/channel",
       "search": "?passwd",
       "hash": ""
      },
      {
       "input": "dns://fw.example.org:9999/foo.bar.org?type=TXT",
       "base": "about:blank",
       "href": "dns://fw.example.org:9999/foo.bar.org?type=TXT",
       "origin": "null",
       "protocol": "dns:",
       "username": "",
       "password": "",
       "host": "fw.example.org:9999",
       "hostname": "fw.example.org",
       "port": "9999",
       "pathname": "/foo.bar.org",
       "search": "?type=TXT",
       "hash": ""
      },
      {
       "input": "ldap://localhost:389/ou=People,o=JNDITutorial",
       "base": "about:blank",
       "href": "ldap://localhost:389/ou=People,o=JNDITutorial",
       "origin": "null",
       "protocol": "ldap:",
       "username": "",
       "password": "",
       "host": "localhost:389",
       "hostname": "localhost",
       "port": "389",
       "pathname": "/ou=People,o=JNDITutorial",
       "search": "",
       "hash": ""
      },
      {
       "input": "git+https://github.com/foo/bar",
       "base": "about:blank",
       "href": "git+https://github.com/foo/bar",
       "origin": "null",
       "protocol": "git+https:",
       "username": "",
       "password": "",
       "host": "github.com",
       "hostname": "github.com",
       "port": "",
       "pathname": "/foo/bar",
       "search": "",
       "hash": ""
      },
      {
       "input": "urn:ietf:rfc:2648",
       "base": "about:blank",
       "href": "urn:ietf:rfc:2648",
       "origin": "null",
       "protocol": "urn:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "ietf:rfc:2648",
       "search": "",
       "hash": ""
      },
      {
       "input": "tag:joe@example.org,2001:foo/bar",
       "base": "about:blank",
       "href": "tag:joe@example.org,2001:foo/bar",
       "origin": "null",
       "protocol": "tag:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "joe@example.org,2001:foo/bar",
       "search": "",
       "hash": ""
      },
      "# percent encoded hosts in non-special-URLs",
      {
       "input": "non-special://%E2%80%A0/",
       "base": "about:blank",
       "href": "non-special://%E2%80%A0/",
       "protocol": "non-special:",
       "username": "",
       "password": "",
       "host": "%E2%80%A0",
       "hostname": "%E2%80%A0",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "non-special://H%4fSt/path",
       "base": "about:blank",
       "href": "non-special://H%4fSt/path",
       "protocol": "non-special:",
       "username": "",
       "password": "",
       "host": "H%4fSt",
       "hostname": "H%4fSt",
       "port": "",
       "pathname": "/path",
       "search": "",
       "hash": ""
      },
      "# IPv6 in non-special-URLs",
      {
       "input": "non-special://[1:2:0:0:5:0:0:0]/",
       "base": "about:blank",
       "href": "non-special://[1:2:0:0:5::]/",
       "protocol": "non-special:",
       "username": "",
       "password": "",
       "host": "[1:2:0:0:5::]",
       "hostname": "[1:2:0:0:5::]",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "non-special://[1:2:0:0:0:0:0:3]/",
       "base": "about:blank",
       "href": "non-special://[1:2::3]/",
       "protocol": "non-special:",
       "username": "",
       "password": "",
       "host": "[1:2::3]",
       "hostname": "[1:2::3]",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "non-special://[1:2::3]:80/",
       "base": "about:blank",
       "href": "non-special://[1:2::3]:80/",
       "protocol": "non-special:",
       "username": "",
       "password": "",
       "host": "[1:2::3]:80",
       "hostname": "[1:2::3]",
       "port": "80",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
        "input": "non-special://[:80/",
        "base": "about:blank",
        "failure": true
      },
      {
       "input": "blob:https://example.com:443/",
       "base": "about:blank",
       "href": "blob:https://example.com:443/",
       "origin": "https://example.com",
       "protocol": "blob:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "https://example.com:443/",
       "search": "",
       "hash": ""
      },
      {
       "input": "blob:d3958f5c-0777-0845-9dcf-2cb28783acaf",
       "base": "about:blank",
       "href": "blob:d3958f5c-0777-0845-9dcf-2cb28783acaf",
       "protocol": "blob:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "d3958f5c-0777-0845-9dcf-2cb28783acaf",
       "search": "",
       "hash": ""
      },
      "Invalid IPv4 radix digits",
      {
       "input": "http://0x7f.0.0.0x7g",
       "base": "about:blank",
       "href": "http://0x7f.0.0.0x7g/",
       "origin": "http://0x7f.0.0.0x7g",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "0x7f.0.0.0x7g",
       "hostname": "0x7f.0.0.0x7g",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://0X7F.0.0.0X7G",
       "base": "about:blank",
       "href": "http://0x7f.0.0.0x7g/",
       "origin": "http://0x7f.0.0.0x7g",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "0x7f.0.0.0x7g",
       "hostname": "0x7f.0.0.0x7g",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "Invalid IPv4 portion of IPv6 address",
      {
        "input": "http://[::127.0.0.0.1]",
        "base": "about:blank",
        "failure": true
      },
      "Uncompressed IPv6 addresses with 0",
      {
       "input": "http://[0:1:0:1:0:1:0:1]",
       "base": "about:blank",
       "href": "http://[0:1:0:1:0:1:0:1]/",
       "origin": "http://[0:1:0:1:0:1:0:1]",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "[0:1:0:1:0:1:0:1]",
       "hostname": "[0:1:0:1:0:1:0:1]",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://[1:0:1:0:1:0:1:0]",
       "base": "about:blank",
       "href": "http://[1:0:1:0:1:0:1:0]/",
       "origin": "http://[1:0:1:0:1:0:1:0]",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "[1:0:1:0:1:0:1:0]",
       "hostname": "[1:0:1:0:1:0:1:0]",
       "port": "",
       "pathname": "/",
       "search": "",
       "hash": ""
      },
      "Percent-encoded query and fragment",
      {
       "input": "http://example.org/test?\u0022",
       "base": "about:blank",
       "href": "http://example.org/test?%22",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/test",
       "search": "?%22",
       "hash": ""
      },
      {
       "input": "http://example.org/test?\u0023",
       "base": "about:blank",
       "href": "http://example.org/test?#",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/test",
       "search": "",
       "hash": ""
      },
      {
       "input": "http://example.org/test?\u003C",
       "base": "about:blank",
       "href": "http://example.org/test?%3C",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/test",
       "search": "?%3C",
       "hash": ""
      },
      {
       "input": "http://example.org/test?\u003E",
       "base": "about:blank",
       "href": "http://example.org/test?%3E",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/test",
       "search": "?%3E",
       "hash": ""
      },
      {
       "input": "http://example.org/test?\u2323",
       "base": "about:blank",
       "href": "http://example.org/test?%E2%8C%A3",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/test",
       "search": "?%E2%8C%A3",
       "hash": ""
      },
      {
       "input": "http://example.org/test?%23%23",
       "base": "about:blank",
       "href": "http://example.org/test?%23%23",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/test",
       "search": "?%23%23",
       "hash": ""
      },
      {
       "input": "http://example.org/test?%GH",
       "base": "about:blank",
       "href": "http://example.org/test?%GH",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/test",
       "search": "?%GH",
       "hash": ""
      },
      {
       "input": "http://example.org/test?a#%EF",
       "base": "about:blank",
       "href": "http://example.org/test?a#%EF",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/test",
       "search": "?a",
       "hash": "#%EF"
      },
      {
       "input": "http://example.org/test?a#%GH",
       "base": "about:blank",
       "href": "http://example.org/test?a#%GH",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/test",
       "search": "?a",
       "hash": "#%GH"
      },
      "Bad bases",
      {
        "input": "test-a.html",
        "base": "a",
        "failure": true
      },
      {
        "input": "test-a-slash.html",
        "base": "a/",
        "failure": true
      },
      {
        "input": "test-a-slash-slash.html",
        "base": "a//",
        "failure": true
      },
      {
        "input": "test-a-colon.html",
        "base": "a:",
        "failure": true
      },
      {
       "input": "test-a-colon-slash.html",
       "base": "a:/",
       "href": "a:/test-a-colon-slash.html",
       "protocol": "a:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/test-a-colon-slash.html",
       "search": "",
       "hash": ""
      },
      {
        "input": "test-a-colon-slash-slash.html",
        "base": "a://",
        "href": "a:///test-a-colon-slash-slash.html",
        "protocol": "a:",
        "username": "",
        "password": "",
        "host": "",
        "hostname": "",
        "port": "",
        "pathname": "/test-a-colon-slash-slash.html",
        "search": "",
        "hash": ""
      },
      {
        "input": "test-a-colon-b.html",
        "base": "a:b",
        "failure": true
      },
      {
       "input": "test-a-colon-slash-b.html",
       "base": "a:/b",
       "href": "a:/test-a-colon-slash-b.html",
       "protocol": "a:",
       "username": "",
       "password": "",
       "host": "",
       "hostname": "",
       "port": "",
       "pathname": "/test-a-colon-slash-b.html",
       "search": "",
       "hash": ""
      },
      {
        "input": "test-a-colon-slash-slash-b.html",
        "base": "a://b",
        "href": "a://b/test-a-colon-slash-slash-b.html",
        "protocol": "a:",
        "username": "",
        "password": "",
        "host": "b",
        "hostname": "b",
        "port": "",
        "pathname": "/test-a-colon-slash-slash-b.html",
        "search": "",
        "hash": ""
      },
      "Null code point in fragment",
      {
       "input": "http://example.org/test?a#b\u0000c",
       "base": "about:blank",
       "href": "http://example.org/test?a#b%00c",
       "origin": "http://example.org",
       "protocol": "http:",
       "username": "",
       "password": "",
       "host": "example.org",
       "hostname": "example.org",
       "port": "",
       "pathname": "/test",
       "search": "?a",
       "hash": "#b%00c"
      }
    ];

    cases.forEach((test) => {
      if (typeof test === 'string') return;
      if (test.failure) {
        throws(() => {
          new URL(test.input, test.base || 'about:blank');
        });
        return;
      }
      const url = new URL(test.input, test.base || 'about:blank');
      strictEqual(url.href, test.href);
      strictEqual(url.protocol, test.protocol);
      strictEqual(url.username, test.username);
      strictEqual(url.password, test.password);
      strictEqual(url.host, test.host);
      strictEqual(url.hostname, test.hostname);
      strictEqual(url.port, test.port);
      strictEqual(url.pathname, test.pathname);
      strictEqual(url.search, test.search);
      if ('searchParams' in test) {
        strictEqual(url.searchParams.toString(), test.searchParams);
      }
      strictEqual(url.hash, test.hash);
      if (test.origin == null) {
        strictEqual(url.origin, "null");
      } else {
        strictEqual(url.origin, test.origin);
      }
    });
  }
};

export const urlSetterTests = {
  test() {

    const cases = {
      "protocol": [
        {
          "comment": "The empty string is not a valid scheme. Setter leaves the URL unchanged.",
          "href": "a://example.net",
          "new_value": "",
          "expected": {
            "href": "a://example.net",
            "protocol": "a:"
          }
        },
        {
          "href": "a://example.net",
          "new_value": "b",
          "expected": {
            "href": "b://example.net",
            "protocol": "b:"
          }
        },
        {
          "href": "javascript:alert(1)",
          "new_value": "defuse",
          "expected": {
            "href": "defuse:alert(1)",
            "protocol": "defuse:"
          }
        },
        {
          "comment": "Upper-case ASCII is lower-cased",
          "href": "a://example.net",
          "new_value": "B",
          "expected": {
            "href": "b://example.net",
            "protocol": "b:"
          }
        },
        {
          "comment": "Non-ASCII is rejected",
          "href": "a://example.net",
          "new_value": "é",
          "expected": {
            "href": "a://example.net",
            "protocol": "a:"
          }
        },
        {
          "comment": "No leading digit",
          "href": "a://example.net",
          "new_value": "0b",
          "expected": {
            "href": "a://example.net",
            "protocol": "a:"
          }
        },
        {
          "comment": "No leading punctuation",
          "href": "a://example.net",
          "new_value": "+b",
          "expected": {
            "href": "a://example.net",
            "protocol": "a:"
          }
        },
        {
          "href": "a://example.net",
          "new_value": "bC0+-.",
          "expected": {
            "href": "bc0+-.://example.net",
            "protocol": "bc0+-.:"
          }
        },
        {
          "comment": "Only some punctuation is acceptable",
          "href": "a://example.net",
          "new_value": "b,c",
          "expected": {
            "href": "a://example.net",
            "protocol": "a:"
          }
        },
        {
          "comment": "Non-ASCII is rejected",
          "href": "a://example.net",
          "new_value": "bé",
          "expected": {
            "href": "a://example.net",
            "protocol": "a:"
          }
        },
        {
          "comment": "Can’t switch from URL containing username/password/port to file",
          "href": "http://test@example.net",
          "new_value": "file",
          "expected": {
            "href": "http://test@example.net/",
            "protocol": "http:"
          }
        },
       {
         "href": "gopher://example.net:1234",
         "new_value": "file",
         "expected": {
           "href": "gopher://example.net:1234",
           "protocol": "gopher:"
         }
       },
       {
         "href": "wss://x:x@example.net:1234",
         "new_value": "file",
         "expected": {
           "href": "wss://x:x@example.net:1234/",
           "protocol": "wss:"
         }
       },
       {
         "comment": "Can’t switch from file URL with no host",
         "href": "file://localhost/",
         "new_value": "http",
         "expected": {
           "href": "file:///",
           "protocol": "file:"
         }
       },
       {
         "href": "file:///test",
         "new_value": "gopher",
         "expected": {
           "href": "file:///test",
           "protocol": "file:"
         }
       },
       {
         "href": "file:",
         "new_value": "wss",
         "expected": {
           "href": "file:///",
           "protocol": "file:"
         }
       },
       {
         "comment": "Can’t switch from special scheme to non-special",
         "href": "http://example.net",
         "new_value": "b",
         "expected": {
           "href": "http://example.net/",
           "protocol": "http:"
         }
       },
       {
         "href": "file://hi/path",
         "new_value": "s",
         "expected": {
           "href": "file://hi/path",
           "protocol": "file:"
         }
       },
       {
         "href": "https://example.net",
         "new_value": "s",
         "expected": {
           "href": "https://example.net/",
           "protocol": "https:"
         }
       },
       {
         "href": "ftp://example.net",
         "new_value": "test",
         "expected": {
           "href": "ftp://example.net/",
           "protocol": "ftp:"
         }
       },
       {
         "comment": "Cannot-be-a-base URL doesn’t have a host, but URL in a special scheme must.",
         "href": "mailto:me@example.net",
         "new_value": "http",
         "expected": {
           "href": "mailto:me@example.net",
           "protocol": "mailto:"
         }
       },
       {
         "comment": "Can’t switch from non-special scheme to special",
         "href": "ssh://me@example.net",
         "new_value": "http",
         "expected": {
           "href": "ssh://me@example.net",
           "protocol": "ssh:"
         }
       },
       {
         "href": "ssh://me@example.net",
         "new_value": "gopher",
         "expected": {
           "href": "gopher://me@example.net",
           "protocol": "gopher:"
         }
       },
       {
         "href": "ssh://me@example.net",
         "new_value": "file",
         "expected": {
           "href": "ssh://me@example.net",
           "protocol": "ssh:"
         }
       },
       {
         "href": "ssh://example.net",
         "new_value": "file",
         "expected": {
           "href": "ssh://example.net",
           "protocol": "ssh:"
         }
       },
       {
         "href": "nonsense:///test",
         "new_value": "https",
         "expected": {
           "href": "nonsense:///test",
           "protocol": "nonsense:"
         }
       },
       {
         "comment": "Stuff after the first ':' is ignored",
         "href": "http://example.net",
         "new_value": "https:foo : bar",
         "expected": {
           "href": "https://example.net/",
           "protocol": "https:"
         }
       },
       {
         "comment": "Stuff after the first ':' is ignored",
         "href": "data:text/html,<p>Test",
         "new_value": "view-source+data:foo : bar",
         "expected": {
           "href": "view-source+data:text/html,<p>Test",
           "protocol": "view-source+data:"
         }
       },
       {
         "comment": "Port is set to null if it is the default for new scheme.",
         "href": "http://foo.com:443/",
         "new_value": "https",
         "expected": {
           "href": "https://foo.com/",
           "protocol": "https:",
           "port": ""
         }
       }
      ],
      "username": [
       {
         "comment": "No host means no username",
         "href": "file:///home/you/index.html",
         "new_value": "me",
         "expected": {
           "href": "file:///home/you/index.html",
           "username": ""
         }
       },
       {
         "comment": "No host means no username",
         "href": "unix:/run/foo.socket",
         "new_value": "me",
         "expected": {
           "href": "unix:/run/foo.socket",
           "username": ""
         }
       },
       {
         "comment": "Cannot-be-a-base means no username",
         "href": "mailto:you@example.net",
         "new_value": "me",
         "expected": {
           "href": "mailto:you@example.net",
           "username": ""
         }
       },
       {
         "href": "javascript:alert(1)",
         "new_value": "wario",
         "expected": {
           "href": "javascript:alert(1)",
           "username": ""
         }
       },
       {
         "href": "http://example.net",
         "new_value": "me",
         "expected": {
           "href": "http://me@example.net/",
           "username": "me"
         }
       },
       {
         "href": "http://:secret@example.net",
         "new_value": "me",
         "expected": {
           "href": "http://me:secret@example.net/",
           "username": "me"
         }
       },
       {
         "href": "http://me@example.net",
         "new_value": "",
         "expected": {
           "href": "http://example.net/",
           "username": ""
         }
       },
       {
         "href": "http://me:secret@example.net",
         "new_value": "",
         "expected": {
           "href": "http://:secret@example.net/",
           "username": ""
         }
       },
       {
         "comment": "UTF-8 percent encoding with the userinfo encode set.",
         "href": "http://example.net",
         "new_value": "\u0000\u0001\t\n\r\u001f !\"#$%&'()*+,-./09:;<=>?@AZ[\\]^_`az{|}~\u007f\u0080\u0081Éé",
         "expected": {
           "href": "http://%00%01%09%0A%0D%1F%20!%22%23$%&'()*+,-.%2F09%3A%3B%3C%3D%3E%3F%40AZ%5B%5C%5D%5E_%60az%7B%7C%7D~%7F%C2%80%C2%81%C3%89%C3%A9@example.net/",
           "username": "%00%01%09%0A%0D%1F%20!%22%23$%&'()*+,-.%2F09%3A%3B%3C%3D%3E%3F%40AZ%5B%5C%5D%5E_%60az%7B%7C%7D~%7F%C2%80%C2%81%C3%89%C3%A9"
         }
       },
       {
         "comment": "Bytes already percent-encoded are left as-is.",
         "href": "http://example.net",
         "new_value": "%c3%89té",
         "expected": {
           "href": "http://%c3%89t%C3%A9@example.net/",
           "username": "%c3%89t%C3%A9"
         }
       },
       {
         "href": "sc:///",
         "new_value": "x",
         "expected": {
           "href": "sc:///",
           "username": ""
         }
       },
        {
          "href": "javascript://x/",
          "new_value": "wario",
          "expected": {
            "href": "javascript://wario@x/",
            "username": "wario"
          }
        },
       {
         "href": "file://test/",
         "new_value": "test",
         "expected": {
           "href": "file://test/",
           "username": ""
         }
       }
      ],
      "password": [
       {
         "comment": "No host means no password",
         "href": "file:///home/me/index.html",
         "new_value": "secret",
         "expected": {
           "href": "file:///home/me/index.html",
           "password": ""
         }
       },
       {
         "comment": "No host means no password",
         "href": "unix:/run/foo.socket",
         "new_value": "secret",
         "expected": {
           "href": "unix:/run/foo.socket",
           "password": ""
         }
       },
       {
         "comment": "Cannot-be-a-base means no password",
         "href": "mailto:me@example.net",
         "new_value": "secret",
         "expected": {
           "href": "mailto:me@example.net",
           "password": ""
         }
       },
       {
         "href": "http://example.net",
         "new_value": "secret",
         "expected": {
           "href": "http://:secret@example.net/",
           "password": "secret"
         }
       },
       {
         "href": "http://me@example.net",
         "new_value": "secret",
         "expected": {
           "href": "http://me:secret@example.net/",
           "password": "secret"
         }
       },
       {
         "href": "http://:secret@example.net",
         "new_value": "",
         "expected": {
           "href": "http://example.net/",
           "password": ""
         }
       },
       {
         "href": "http://me:secret@example.net",
         "new_value": "",
         "expected": {
           "href": "http://me@example.net/",
           "password": ""
         }
       },
       {
         "comment": "UTF-8 percent encoding with the userinfo encode set.",
         "href": "http://example.net",
         "new_value": "\u0000\u0001\t\n\r\u001f !\"#$%&'()*+,-./09:;<=>?@AZ[\\]^_`az{|}~\u007f\u0080\u0081Éé",
         "expected": {
           "href": "http://:%00%01%09%0A%0D%1F%20!%22%23$%&'()*+,-.%2F09%3A%3B%3C%3D%3E%3F%40AZ%5B%5C%5D%5E_%60az%7B%7C%7D~%7F%C2%80%C2%81%C3%89%C3%A9@example.net/",
           "password": "%00%01%09%0A%0D%1F%20!%22%23$%&'()*+,-.%2F09%3A%3B%3C%3D%3E%3F%40AZ%5B%5C%5D%5E_%60az%7B%7C%7D~%7F%C2%80%C2%81%C3%89%C3%A9"
         }
       },
       {
         "comment": "Bytes already percent-encoded are left as-is.",
         "href": "http://example.net",
         "new_value": "%c3%89té",
         "expected": {
           "href": "http://:%c3%89t%C3%A9@example.net/",
           "password": "%c3%89t%C3%A9"
         }
       },
       {
         "href": "sc:///",
         "new_value": "x",
         "expected": {
           "href": "sc:///",
           "password": ""
         }
       },
        {
          "href": "javascript://x/",
          "new_value": "bowser",
          "expected": {
            "href": "javascript://:bowser@x/",
            "password": "bowser"
          }
        },
       {
         "href": "file://test/",
         "new_value": "test",
         "expected": {
           "href": "file://test/",
           "password": ""
         }
       }
      ],
      "host": [
       {
         "comment": "Non-special scheme",
         "href": "sc://x/",
         "new_value": "\u0000",
         "expected": {
           "href": "sc://x/",
           "host": "x",
           "hostname": "x"
         }
       },
       {
         "href": "sc://x/",
         "new_value": "\u0009",
         "expected": {
           "href": "sc:///",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "sc://x/",
         "new_value": "\u000A",
         "expected": {
           "href": "sc:///",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "sc://x/",
         "new_value": "\u000D",
         "expected": {
           "href": "sc:///",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "sc://x/",
         "new_value": " ",
         "expected": {
           "href": "sc://x/",
           "host": "x",
           "hostname": "x"
         }
       },
       {
         "href": "sc://x/",
         "new_value": "#",
         "expected": {
           "href": "sc:///",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "sc://x/",
         "new_value": "/",
         "expected": {
           "href": "sc:///",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "sc://x/",
         "new_value": "?",
         "expected": {
           "href": "sc:///",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "sc://x/",
         "new_value": "@",
         "expected": {
           "href": "sc://x/",
           "host": "x",
           "hostname": "x"
         }
       },
       {
         "href": "sc://x/",
         "new_value": "ß",
         "expected": {
           "href": "sc://%C3%9F/",
           "host": "%C3%9F",
           "hostname": "%C3%9F"
         }
       },
       {
         "comment": "IDNA Nontransitional_Processing",
         "href": "https://x/",
         "new_value": "ß",
         "expected": {
           "href": "https://xn--zca/",
           "host": "xn--zca",
           "hostname": "xn--zca"
         }
       },
       {
         "comment": "Cannot-be-a-base means no host",
         "href": "mailto:me@example.net",
         "new_value": "example.com",
         "expected": {
           "href": "mailto:me@example.net",
           "host": ""
         }
       },
       {
         "comment": "Cannot-be-a-base means no password",
         "href": "data:text/plain,Stuff",
         "new_value": "example.net",
         "expected": {
           "href": "data:text/plain,Stuff",
           "host": ""
         }
       },
       {
         "href": "http://example.net",
         "new_value": "example.com:8080",
         "expected": {
           "href": "http://example.com:8080/",
           "host": "example.com:8080",
           "hostname": "example.com",
           "port": "8080"
         }
       },
       {
         "comment": "Port number is unchanged if not specified in the new value",
         "href": "http://example.net:8080",
         "new_value": "example.com",
         "expected": {
           "href": "http://example.com:8080/",
           "host": "example.com:8080",
           "hostname": "example.com",
           "port": "8080"
         }
       },
       {
         "comment": "Port number is unchanged if not specified",
         "href": "http://example.net:8080",
         "new_value": "example.com:",
         "expected": {
           "href": "http://example.com:8080/",
           "host": "example.com:8080",
           "hostname": "example.com",
           "port": "8080"
         }
       },
       {
         "comment": "The empty host is not valid for special schemes",
         "href": "http://example.net",
         "new_value": "",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net"
         }
       },
        {
          "comment": "The empty host is OK for non-special schemes",
          "href": "view-source+http://example.net/foo",
          "new_value": "",
          "expected": {
            "href": "view-source+http:///foo",
            "host": ""
          }
        },
       {
         "comment": "Path-only URLs can gain a host",
         "href": "a:/foo",
         "new_value": "example.net",
         "expected": {
           "href": "a://example.net/foo",
           "host": "example.net"
         }
       },
       {
         "comment": "IPv4 address syntax is normalized",
         "href": "http://example.net",
         "new_value": "0x7F000001:8080",
         "expected": {
           "href": "http://127.0.0.1:8080/",
           "host": "127.0.0.1:8080",
           "hostname": "127.0.0.1",
           "port": "8080"
         }
       },
       {
         "comment": "IPv6 address syntax is normalized",
         "href": "http://example.net",
         "new_value": "[::0:01]:2",
         "expected": {
           "href": "http://[::1]:2/",
           "host": "[::1]:2",
           "hostname": "[::1]",
           "port": "2"
         }
       },
       {
         "comment": "Default port number is removed",
         "href": "http://example.net",
         "new_value": "example.com:80",
         "expected": {
           "href": "http://example.com/",
           "host": "example.com",
           "hostname": "example.com",
           "port": ""
         }
       },
       {
         "comment": "Default port number is removed",
         "href": "https://example.net",
         "new_value": "example.com:443",
         "expected": {
           "href": "https://example.com/",
           "host": "example.com",
           "hostname": "example.com",
           "port": ""
         }
       },
       {
         "comment": "Default port number is only removed for the relevant scheme",
         "href": "https://example.net",
         "new_value": "example.com:80",
         "expected": {
           "href": "https://example.com:80/",
           "host": "example.com:80",
           "hostname": "example.com",
           "port": "80"
         }
       },
       {
         "comment": "Stuff after a / delimiter is ignored",
         "href": "http://example.net/path",
         "new_value": "example.com/stuff",
         "expected": {
           "href": "http://example.com/path",
           "host": "example.com",
           "hostname": "example.com",
           "port": ""
         }
       },
       {
         "comment": "Stuff after a / delimiter is ignored",
         "href": "http://example.net/path",
         "new_value": "example.com:8080/stuff",
         "expected": {
           "href": "http://example.com:8080/path",
           "host": "example.com:8080",
           "hostname": "example.com",
           "port": "8080"
         }
       },
       {
         "comment": "Stuff after a ? delimiter is ignored",
         "href": "http://example.net/path",
         "new_value": "example.com?stuff",
         "expected": {
           "href": "http://example.com/path",
           "host": "example.com",
           "hostname": "example.com",
           "port": ""
         }
       },
       {
         "comment": "Stuff after a ? delimiter is ignored",
         "href": "http://example.net/path",
         "new_value": "example.com:8080?stuff",
         "expected": {
           "href": "http://example.com:8080/path",
           "host": "example.com:8080",
           "hostname": "example.com",
           "port": "8080"
         }
       },
       {
         "comment": "Stuff after a # delimiter is ignored",
         "href": "http://example.net/path",
         "new_value": "example.com#stuff",
         "expected": {
           "href": "http://example.com/path",
           "host": "example.com",
           "hostname": "example.com",
           "port": ""
         }
       },
       {
         "comment": "Stuff after a # delimiter is ignored",
         "href": "http://example.net/path",
         "new_value": "example.com:8080#stuff",
         "expected": {
           "href": "http://example.com:8080/path",
           "host": "example.com:8080",
           "hostname": "example.com",
           "port": "8080"
         }
       },
       {
         "comment": "Stuff after a \\ delimiter is ignored for special schemes",
         "href": "http://example.net/path",
         "new_value": "example.com\\stuff",
         "expected": {
           "href": "http://example.com/path",
           "host": "example.com",
           "hostname": "example.com",
           "port": ""
         }
       },
       {
         "comment": "Stuff after a \\ delimiter is ignored for special schemes",
         "href": "http://example.net/path",
         "new_value": "example.com:8080\\stuff",
         "expected": {
           "href": "http://example.com:8080/path",
           "host": "example.com:8080",
           "hostname": "example.com",
           "port": "8080"
         }
       },
       {
         "comment": "\\ is not a delimiter for non-special schemes, but still forbidden in hosts",
         "href": "view-source+http://example.net/path",
         "new_value": "example.com\\stuff",
         "expected": {
           "href": "view-source+http://example.net/path",
           "host": "example.net",
           "hostname": "example.net",
           "port": ""
         }
       },
       {
         "comment": "Anything other than ASCII digit stops the port parser in a setter but is not an error",
         "href": "view-source+http://example.net/path",
         "new_value": "example.com:8080stuff2",
         "expected": {
           "href": "view-source+http://example.com:8080/path",
           "host": "example.com:8080",
           "hostname": "example.com",
           "port": "8080"
         }
       },
       {
         "comment": "Anything other than ASCII digit stops the port parser in a setter but is not an error",
         "href": "http://example.net/path",
         "new_value": "example.com:8080stuff2",
         "expected": {
           "href": "http://example.com:8080/path",
           "host": "example.com:8080",
           "hostname": "example.com",
           "port": "8080"
         }
       },
       {
         "comment": "Anything other than ASCII digit stops the port parser in a setter but is not an error",
         "href": "http://example.net/path",
         "new_value": "example.com:8080+2",
         "expected": {
           "href": "http://example.com:8080/path",
           "host": "example.com:8080",
           "hostname": "example.com",
           "port": "8080"
         }
       },
        {
          "comment": "Port numbers are 16 bit integers",
          "href": "http://example.net/path",
          "new_value": "example.com:65535",
          "expected": {
            "href": "http://example.com:65535/path",
            "host": "example.com:65535",
            "hostname": "example.com",
            "port": "65535"
          }
        },
       {
         "comment": "Port numbers are 16 bit integers, overflowing is an error. Hostname is still set, though.",
         "href": "http://example.net/path",
         "new_value": "example.com:65536",
         "expected": {
           "href": "http://example.com/path",
           "host": "example.com",
           "hostname": "example.com",
           "port": ""
         }
       },
       {
         "comment": "Broken IPv6",
         "href": "http://example.net/",
         "new_value": "[google.com]",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net"
         }
       },
       {
         "href": "http://example.net/",
         "new_value": "[::1.2.3.4x]",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net"
         }
       },
       {
         "href": "http://example.net/",
         "new_value": "[::1.2.3.]",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net"
         }
       },
       {
         "href": "http://example.net/",
         "new_value": "[::1.2.]",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net"
         }
       },
       {
         "href": "http://example.net/",
         "new_value": "[::1.]",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net"
         }
       },
       {
         "href": "file://y/",
         "new_value": "x:123",
         "expected": {
           "href": "file://y/",
           "host": "y",
           "hostname": "y",
           "port": ""
         }
       },
       {
         "href": "file://y/",
         "new_value": "loc%41lhost",
         "expected": {
           "href": "file:///",
           "host": "",
           "hostname": "",
           "port": ""
         }
       },
        {
          "href": "file://hi/x",
          "new_value": "",
          "expected": {
            "href": "file:///x",
            "host": "",
            "hostname": "",
            "port": ""
          }
        },
       {
         "href": "sc://test@test/",
         "new_value": "",
         "expected": {
           "href": "sc://test@test/",
           "host": "test",
           "hostname": "test",
           "username": "test"
         }
       },
       {
         "href": "sc://test:12/",
         "new_value": "",
         "expected": {
           "href": "sc://test:12/",
           "host": "test:12",
           "hostname": "test",
           "port": "12"
         }
       }
      ],

      "hostname": [
       {
         "comment": "Non-special scheme",
         "href": "sc://x/",
         "new_value": "\u0000",
         "expected": {
           "href": "sc://x/",
           "host": "x",
           "hostname": "x"
         }
       },
       {
         "href": "sc://x/",
         "new_value": "\u0009",
         "expected": {
           "href": "sc:///",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "sc://x/",
         "new_value": "\u000A",
         "expected": {
           "href": "sc:///",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "sc://x/",
         "new_value": "\u000D",
         "expected": {
           "href": "sc:///",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "sc://x/",
         "new_value": " ",
         "expected": {
           "href": "sc://x/",
           "host": "x",
           "hostname": "x"
         }
       },
       {
         "href": "sc://x/",
         "new_value": "#",
         "expected": {
           "href": "sc:///",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "sc://x/",
         "new_value": "/",
         "expected": {
           "href": "sc:///",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "sc://x/",
         "new_value": "?",
         "expected": {
           "href": "sc:///",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "sc://x/",
         "new_value": "@",
         "expected": {
           "href": "sc://x/",
           "host": "x",
           "hostname": "x"
         }
       },
       {
         "comment": "Cannot-be-a-base means no host",
         "href": "mailto:me@example.net",
         "new_value": "example.com",
         "expected": {
           "href": "mailto:me@example.net",
           "host": "",
           "hostname": "",
         }
       },
       {
         "comment": "Cannot-be-a-base means no password",
         "href": "data:text/plain,Stuff",
         "new_value": "example.net",
         "expected": {
           "href": "data:text/plain,Stuff",
           "host": "",
           "hostname": ""
         }
       },
       {
         "href": "http://example.net:8080",
         "new_value": "example.com",
         "expected": {
           "href": "http://example.com:8080/",
           "host": "example.com:8080",
           "hostname": "example.com",
           "port": "8080"
         }
       },
       {
         "comment": "The empty host is not valid for special schemes",
         "href": "http://example.net",
         "new_value": "",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net"
         }
       },
        {
          "comment": "The empty host is OK for non-special schemes",
          "href": "view-source+http://example.net/foo",
          "new_value": "",
          "expected": {
            "href": "view-source+http:///foo",
            "host": "",
            "hostname": "",
          }
        },
       {
         "comment": "Path-only URLs can gain a host",
         "href": "a:/foo",
         "new_value": "example.net",
         "expected": {
           "href": "a://example.net/foo",
           "host": "example.net",
           "hostname": "example.net"
         }
       },
       {
         "comment": "IPv4 address syntax is normalized",
         "href": "http://example.net:8080",
         "new_value": "0x7F000001",
         "expected": {
           "href": "http://127.0.0.1:8080/",
           "host": "127.0.0.1:8080",
           "hostname": "127.0.0.1",
           "port": "8080"
         }
       },
       {
         "comment": "IPv6 address syntax is normalized",
         "href": "http://example.net",
         "new_value": "[::0:01]",
         "expected": {
           "href": "http://[::1]/",
           "host": "[::1]",
           "hostname": "[::1]",
           "port": ""
         }
       },
          {
              "comment": ": delimiter invalidates entire value",
              "href": "http://example.net/path",
              "new_value": "example.com:8080",
              "expected": {
                  "href": "http://example.net/path",
                  "host": "example.net",
                  "hostname": "example.net",
                  "port": ""
              }
          },
          {
              "comment": ": delimiter invalidates entire value",
              "href": "http://example.net:8080/path",
              "new_value": "example.com:",
              "expected": {
                  "href": "http://example.net:8080/path",
                  "host": "example.net:8080",
                  "hostname": "example.net",
                  "port": "8080"
              }
          },
       {
         "comment": "Stuff after a / delimiter is ignored",
         "href": "http://example.net/path",
         "new_value": "example.com/stuff",
         "expected": {
           "href": "http://example.com/path",
           "host": "example.com",
           "hostname": "example.com",
           "port": ""
         }
       },
       {
         "comment": "Stuff after a ? delimiter is ignored",
         "href": "http://example.net/path",
         "new_value": "example.com?stuff",
         "expected": {
           "href": "http://example.com/path",
           "host": "example.com",
           "hostname": "example.com",
           "port": ""
         }
       },
       {
         "comment": "Stuff after a # delimiter is ignored",
         "href": "http://example.net/path",
         "new_value": "example.com#stuff",
         "expected": {
           "href": "http://example.com/path",
           "host": "example.com",
           "hostname": "example.com",
           "port": ""
         }
       },
       {
         "comment": "Stuff after a \\ delimiter is ignored for special schemes",
         "href": "http://example.net/path",
         "new_value": "example.com\\stuff",
         "expected": {
           "href": "http://example.com/path",
           "host": "example.com",
           "hostname": "example.com",
           "port": ""
         }
       },
       {
         "comment": "\\ is not a delimiter for non-special schemes, but still forbidden in hosts",
         "href": "view-source+http://example.net/path",
         "new_value": "example.com\\stuff",
         "expected": {
           "href": "view-source+http://example.net/path",
           "host": "example.net",
           "hostname": "example.net",
           "port": ""
         }
       },
       {
         "comment": "Broken IPv6",
         "href": "http://example.net/",
         "new_value": "[google.com]",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net"
         }
       },
       {
         "href": "http://example.net/",
         "new_value": "[::1.2.3.4x]",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net"
         }
       },
       {
         "href": "http://example.net/",
         "new_value": "[::1.2.3.]",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net"
         }
       },
       {
         "href": "http://example.net/",
         "new_value": "[::1.2.]",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net"
         }
       },
       {
         "href": "http://example.net/",
         "new_value": "[::1.]",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net"
         }
       },
       {
         "href": "file://y/",
         "new_value": "x:123",
         "expected": {
           "href": "file://y/",
           "host": "y",
           "hostname": "y",
           "port": ""
         }
       },
       {
         "href": "file://y/",
         "new_value": "loc%41lhost",
         "expected": {
           "href": "file:///",
           "host": "",
           "hostname": "",
           "port": ""
         }
       },
        {
          "href": "file://hi/x",
          "new_value": "",
          "expected": {
            "href": "file:///x",
            "host": "",
            "hostname": "",
            "port": ""
          }
        },
       {
         "href": "sc://test@test/",
         "new_value": "",
         "expected": {
           "href": "sc://test@test/",
           "host": "test",
           "hostname": "test",
           "username": "test"
         }
       },
       {
         "href": "sc://test:12/",
         "new_value": "",
         "expected": {
           "href": "sc://test:12/",
           "host": "test:12",
           "hostname": "test",
           "port": "12"
         }
       }
      ],
      "port": [
       {
         "href": "http://example.net",
         "new_value": "8080",
         "expected": {
           "href": "http://example.net:8080/",
           "host": "example.net:8080",
           "hostname": "example.net",
           "port": "8080"
         }
       },
       {
         "comment": "Port number is removed if empty is the new value",
         "href": "http://example.net:8080",
         "new_value": "",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net",
           "port": ""
         }
       },
       {
         "comment": "Default port number is removed",
         "href": "http://example.net:8080",
         "new_value": "80",
         "expected": {
           "href": "http://example.net/",
           "host": "example.net",
           "hostname": "example.net",
           "port": ""
         }
       },
       {
         "comment": "Default port number is removed",
         "href": "https://example.net:4433",
         "new_value": "443",
         "expected": {
           "href": "https://example.net/",
           "host": "example.net",
           "hostname": "example.net",
           "port": ""
         }
       },
       {
         "comment": "Default port number is only removed for the relevant scheme",
         "href": "https://example.net",
         "new_value": "80",
         "expected": {
           "href": "https://example.net:80/",
           "host": "example.net:80",
           "hostname": "example.net",
           "port": "80"
         }
       },
       {
         "comment": "Stuff after a / delimiter is ignored",
         "href": "http://example.net/path",
         "new_value": "8080/stuff",
         "expected": {
           "href": "http://example.net:8080/path",
           "host": "example.net:8080",
           "hostname": "example.net",
           "port": "8080"
         }
       },
       {
         "comment": "Stuff after a ? delimiter is ignored",
         "href": "http://example.net/path",
         "new_value": "8080?stuff",
         "expected": {
           "href": "http://example.net:8080/path",
           "host": "example.net:8080",
           "hostname": "example.net",
           "port": "8080"
         }
       },
       {
         "comment": "Stuff after a # delimiter is ignored",
         "href": "http://example.net/path",
         "new_value": "8080#stuff",
         "expected": {
           "href": "http://example.net:8080/path",
           "host": "example.net:8080",
           "hostname": "example.net",
           "port": "8080"
         }
       },
       {
         "comment": "Stuff after a \\ delimiter is ignored for special schemes",
         "href": "http://example.net/path",
         "new_value": "8080\\stuff",
         "expected": {
           "href": "http://example.net:8080/path",
           "host": "example.net:8080",
           "hostname": "example.net",
           "port": "8080"
         }
       },
       {
         "comment": "Anything other than ASCII digit stops the port parser in a setter but is not an error",
         "href": "view-source+http://example.net/path",
         "new_value": "8080stuff2",
         "expected": {
           "href": "view-source+http://example.net:8080/path",
           "host": "example.net:8080",
           "hostname": "example.net",
           "port": "8080"
         }
       },
       {
         "comment": "Anything other than ASCII digit stops the port parser in a setter but is not an error",
         "href": "http://example.net/path",
         "new_value": "8080stuff2",
         "expected": {
           "href": "http://example.net:8080/path",
           "host": "example.net:8080",
           "hostname": "example.net",
           "port": "8080"
         }
       },
       {
         "comment": "Anything other than ASCII digit stops the port parser in a setter but is not an error",
         "href": "http://example.net/path",
         "new_value": "8080+2",
         "expected": {
           "href": "http://example.net:8080/path",
           "host": "example.net:8080",
           "hostname": "example.net",
           "port": "8080"
         }
       },
        {
          "comment": "Port numbers are 16 bit integers",
          "href": "http://example.net/path",
          "new_value": "65535",
          "expected": {
            "href": "http://example.net:65535/path",
            "host": "example.net:65535",
            "hostname": "example.net",
            "port": "65535"
          }
        },
       {
         "comment": "Port numbers are 16 bit integers, overflowing is an error",
         "href": "http://example.net:8080/path",
         "new_value": "65536",
         "expected": {
           "href": "http://example.net:8080/path",
           "host": "example.net:8080",
           "hostname": "example.net",
           "port": "8080"
         }
       },
       {
         "comment": "Port numbers are 16 bit integers, overflowing is an error",
         "href": "non-special://example.net:8080/path",
         "new_value": "65536",
         "expected": {
           "href": "non-special://example.net:8080/path",
           "host": "example.net:8080",
           "hostname": "example.net",
           "port": "8080"
         }
       },
       {
         "href": "file://test/",
         "new_value": "12",
         "expected": {
           "href": "file://test/",
           "port": ""
         }
       },
       {
         "href": "file://localhost/",
         "new_value": "12",
         "expected": {
           "href": "file:///",
           "port": ""
         }
       },
       {
         "href": "non-base:value",
         "new_value": "12",
         "expected": {
           "href": "non-base:value",
           "port": ""
         }
       },
       {
         "href": "sc:///",
         "new_value": "12",
         "expected": {
           "href": "sc:///",
           "port": ""
         }
       },
        {
          "href": "sc://x/",
          "new_value": "12",
          "expected": {
            "href": "sc://x:12/",
            "port": "12"
          }
        },
        {
          "href": "javascript://x/",
          "new_value": "12",
          "expected": {
            "href": "javascript://x:12/",
            "port": "12"
          }
        }
      ],
      "pathname": [
       {
         "comment": "Cannot-be-a-base don’t have a path",
         "href": "mailto:me@example.net",
         "new_value": "/foo",
         "expected": {
           "href": "mailto:me@example.net",
           "pathname": "me@example.net"
         }
       },
       {
         "href": "unix:/run/foo.socket?timeout=10",
         "new_value": "/var/log/../run/bar.socket",
         "expected": {
           "href": "unix:/var/run/bar.socket?timeout=10",
           "pathname": "/var/run/bar.socket"
         }
       },
        {
          "href": "https://example.net#nav",
          "new_value": "home",
          "expected": {
            "href": "https://example.net/home#nav",
            "pathname": "/home"
          }
        },
        {
          "href": "https://example.net#nav",
          "new_value": "../home",
          "expected": {
            "href": "https://example.net/home#nav",
            "pathname": "/home"
          }
        },
       {
         "comment": "\\ is a segment delimiter for 'special' URLs",
         "href": "http://example.net/home?lang=fr#nav",
         "new_value": "\\a\\%2E\\b\\%2e.\\c",
         "expected": {
           "href": "http://example.net/a/c?lang=fr#nav",
           "pathname": "/a/c"
         }
       },
       {
         "comment": "\\ is *not* a segment delimiter for non-'special' URLs",
         "href": "view-source+http://example.net/home?lang=fr#nav",
         "new_value": "\\a\\%2E\\b\\%2e.\\c",
         "expected": {
           "href": "view-source+http://example.net/\\a\\%2E\\b\\%2e.\\c?lang=fr#nav",
           "pathname": "/\\a\\%2E\\b\\%2e.\\c"
         }
       },
       {
         "comment": "UTF-8 percent encoding with the default encode set. Tabs and newlines are removed.",
         "href": "a:/",
         "new_value": "\u0000\u0001\t\n\r\u001f !\"#$%&'()*+,-./09:;<=>?@AZ[\\]^_`az{|}~\u007f\u0080\u0081Éé",
         "expected": {
           "href": "a:/%00%01%1F%20!%22%23$%&'()*+,-./09:;%3C=%3E%3F@AZ[\\]^_%60az%7B|%7D~%7F%C2%80%C2%81%C3%89%C3%A9",
           "pathname": "/%00%01%1F%20!%22%23$%&'()*+,-./09:;%3C=%3E%3F@AZ[\\]^_%60az%7B|%7D~%7F%C2%80%C2%81%C3%89%C3%A9"
         }
       },
       {
         "comment": "Bytes already percent-encoded are left as-is, including %2E outside dotted segments.",
         "href": "http://example.net",
         "new_value": "%2e%2E%c3%89té",
         "expected": {
           "href": "http://example.net/%2e%2E%c3%89t%C3%A9",
           "pathname": "/%2e%2E%c3%89t%C3%A9"
         }
       },
        {
          "comment": "? needs to be encoded",
          "href": "http://example.net",
          "new_value": "?",
          "expected": {
            "href": "http://example.net/%3F",
            "pathname": "/%3F"
          }
        },
        {
          "comment": "# needs to be encoded",
          "href": "http://example.net",
          "new_value": "#",
          "expected": {
            "href": "http://example.net/%23",
            "pathname": "/%23"
          }
        },
        {
          "comment": "? needs to be encoded, non-special scheme",
          "href": "sc://example.net",
          "new_value": "?",
          "expected": {
            "href": "sc://example.net/%3F",
            "pathname": "/%3F"
          }
        },
        {
          "comment": "# needs to be encoded, non-special scheme",
          "href": "sc://example.net",
          "new_value": "#",
          "expected": {
            "href": "sc://example.net/%23",
            "pathname": "/%23"
          }
        },
       {
         "comment": "File URLs and (back)slashes",
         "href": "file://monkey/",
         "new_value": "\\\\",
         "expected": {
           "href": "file://monkey//",
           "pathname": "//"
         }
       },
       {
         "comment": "File URLs and (back)slashes",
         "href": "file:///unicorn",
         "new_value": "//\\/",
         "expected": {
           "href": "file://////",
           "pathname": "////"
         }
       },
        {
          "comment": "File URLs and (back)slashes",
          "href": "file:///unicorn",
          "new_value": "//monkey/..//",
          "expected": {
            "href": "file://///",
            "pathname": "///"
          }
        }
      ],
      "search": [
       {
         "href": "https://example.net#nav",
         "new_value": "lang=fr",
         "expected": {
           "href": "https://example.net/?lang=fr#nav",
           "search": "?lang=fr"
         }
       },
       {
         "href": "https://example.net?lang=en-US#nav",
         "new_value": "lang=fr",
         "expected": {
           "href": "https://example.net/?lang=fr#nav",
           "search": "?lang=fr"
         }
       },
       {
         "href": "https://example.net?lang=en-US#nav",
         "new_value": "?lang=fr",
         "expected": {
           "href": "https://example.net/?lang=fr#nav",
           "search": "?lang=fr"
         }
       },
       {
         "href": "https://example.net?lang=en-US#nav",
         "new_value": "??lang=fr",
         "expected": {
           "href": "https://example.net/??lang=fr#nav",
           "search": "??lang=fr"
         }
       },
       {
         "href": "https://example.net?lang=en-US#nav",
         "new_value": "?",
         "expected": {
           "href": "https://example.net/?#nav",
           "search": ""
         }
       },
       {
         "href": "https://example.net?lang=en-US#nav",
         "new_value": "",
         "expected": {
           "href": "https://example.net/#nav",
           "search": ""
         }
       },
       {
         "href": "https://example.net?lang=en-US",
         "new_value": "",
         "expected": {
           "href": "https://example.net/",
           "search": ""
         }
       },
       {
         "href": "https://example.net",
         "new_value": "",
         "expected": {
           "href": "https://example.net/",
           "search": ""
         }
       },
       {
         "comment": "UTF-8 percent encoding with the query encode set. Tabs and newlines are removed.",
         "href": "a:/",
         "new_value": "\u0000\u0001\t\n\r\u001f !\"#$%&'()*+,-./09:;<=>?@AZ[\\]^_`az{|}~\u007f\u0080\u0081Éé",
         "expected": {
           "href": "a:/?%00%01%1F%20!%22%23$%&'()*+,-./09:;%3C=%3E?@AZ[\\]^_`az{|}~%7F%C2%80%C2%81%C3%89%C3%A9",
           "search": "?%00%01%1F%20!%22%23$%&'()*+,-./09:;%3C=%3E?@AZ[\\]^_`az{|}~%7F%C2%80%C2%81%C3%89%C3%A9"
         }
       },
       {
         "comment": "Bytes already percent-encoded are left as-is",
         "href": "http://example.net",
         "new_value": "%c3%89té",
         "expected": {
           "href": "http://example.net/?%c3%89t%C3%A9",
           "search": "?%c3%89t%C3%A9"
         }
       }
      ],
      "hash": [
       {
         "href": "https://example.net",
         "new_value": "main",
         "expected": {
           "href": "https://example.net/#main",
           "hash": "#main"
         }
       },
       {
         "href": "https://example.net#nav",
         "new_value": "main",
         "expected": {
           "href": "https://example.net/#main",
           "hash": "#main"
         }
       },
       {
         "href": "https://example.net?lang=en-US",
         "new_value": "##nav",
         "expected": {
           "href": "https://example.net/?lang=en-US##nav",
           "hash": "##nav"
         }
       },
       {
         "href": "https://example.net?lang=en-US#nav",
         "new_value": "#main",
         "expected": {
           "href": "https://example.net/?lang=en-US#main",
           "hash": "#main"
         }
       },
       {
         "href": "https://example.net?lang=en-US#nav",
         "new_value": "#",
         "expected": {
           "href": "https://example.net/?lang=en-US#",
           "hash": ""
         }
       },
       {
         "href": "https://example.net?lang=en-US#nav",
         "new_value": "",
         "expected": {
           "href": "https://example.net/?lang=en-US",
           "hash": ""
         }
       },
       {
         "comment": "Simple percent-encoding; nuls, tabs, and newlines are removed",
         "href": "a:/",
         "new_value": "\u0000\u0001\t\n\r\u001f !\"#$%&'()*+,-./09:;<=>?@AZ[\\]^_`az{|}~\u007f\u0080\u0081Éé",
         "expected": {
           "href": "a:/#%00%01%1F%20!%22#$%&'()*+,-./09:;%3C=%3E?@AZ[\\]^_%60az{|}~%7F%C2%80%C2%81%C3%89%C3%A9",
           "hash": "#%00%01%1F%20!%22#$%&'()*+,-./09:;%3C=%3E?@AZ[\\]^_%60az{|}~%7F%C2%80%C2%81%C3%89%C3%A9"
         }
       },
       {
         "comment": "Bytes already percent-encoded are left as-is",
         "href": "http://example.net",
         "new_value": "%c3%89té",
         "expected": {
           "href": "http://example.net/#%c3%89t%C3%A9",
           "hash": "#%c3%89t%C3%A9"
         }
       },
       {
         "href": "javascript:alert(1)",
         "new_value": "castle",
         "expected": {
           "href": "javascript:alert(1)#castle",
           "hash": "#castle"
         }
       }
      ]
    };

    for (const attribute in cases) {
      cases[attribute].forEach((test) => {
        const url = new URL(test.href);
        url[attribute] = test.new_value;
        strictEqual(url.href, test.expected.href);
        strictEqual(url[attribute], test.expected[attribute]);
      });
    }
  }
};

export const wptTestURLSearchParamsConstructor = {
  test() {
    let params = new URLSearchParams();
    strictEqual(params + '', '');
    params = new URLSearchParams('');
    strictEqual(params + '', '');
    params = new URLSearchParams('a=b');
    strictEqual(params + '', 'a=b');
    params = new URLSearchParams(params);
    strictEqual(params + '', 'a=b');

    {
      const params = new URLSearchParams()
      strictEqual(params.toString(), "")
    }

    {
      const params = new URLSearchParams(DOMException);
      strictEqual(params.toString(), "INDEX_SIZE_ERR=1&DOMSTRING_SIZE_ERR=2&HIERARCHY_REQUEST_ERR=3&WRONG_DOCUMENT_ERR=4&INVALID_CHARACTER_ERR=5&NO_DATA_ALLOWED_ERR=6&NO_MODIFICATION_ALLOWED_ERR=7&NOT_FOUND_ERR=8&NOT_SUPPORTED_ERR=9&INUSE_ATTRIBUTE_ERR=10&INVALID_STATE_ERR=11&SYNTAX_ERR=12&INVALID_MODIFICATION_ERR=13&NAMESPACE_ERR=14&INVALID_ACCESS_ERR=15&VALIDATION_ERR=16&TYPE_MISMATCH_ERR=17&SECURITY_ERR=18&NETWORK_ERR=19&ABORT_ERR=20&URL_MISMATCH_ERR=21&QUOTA_EXCEEDED_ERR=22&TIMEOUT_ERR=23&INVALID_NODE_TYPE_ERR=24&DATA_CLONE_ERR=25")
    }

    {
      const params = new URLSearchParams('');
      strictEqual(params.__proto__, URLSearchParams.prototype);
    }

    {
      const params = new URLSearchParams({});
      strictEqual(params + '', "");
    }

    {
      let params = new URLSearchParams('a=b');
      ok(params.has('a'));
      ok(!params.has('b'));
      params = new URLSearchParams('a=b&c');
      ok(params.has('a'));
      ok(params.has('c'));
      params = new URLSearchParams('&a&&& &&&&&a+b=& c&m%c3%b8%c3%b8');
      ok(params.has('a'));
      ok(params.has('a b'));
      ok(params.has(' '));
      ok(!params.has('c'));
      ok(params.has(' c'));
      ok(params.has('møø'));
    };

    {
      const seed = new URLSearchParams('a=b&c=d');
      const params =  new URLSearchParams(seed);
      strictEqual(params.get('a'), 'b');
      strictEqual(params.get('c'), 'd');
      ok(!params.has('d'));
      // The name-value pairs are copied when created; later updates
      // should not be observable.
      seed.append('e', 'f');
      ok(!params.has('e'));
      params.append('g', 'h');
      ok(!seed.has('g'));
    }

    {
      let params =  new URLSearchParams('a=b+c');
      strictEqual(params.get('a'), 'b c');
      params = new URLSearchParams('a+b=c');
      strictEqual(params.get('a b'), 'c');
    }

    {
      const testValue = '+15555555555';
      const params = new URLSearchParams();
      params.set('query', testValue);
      const newParams = new URLSearchParams(params.toString());

      strictEqual(params.toString(), 'query=%2B15555555555');
      strictEqual(params.get('query'), testValue);
      strictEqual(newParams.get('query'), testValue);
    }

    {
      let params =  new URLSearchParams('a=b c');
      strictEqual(params.get('a'), 'b c');
      params = new URLSearchParams('a b=c');
      strictEqual(params.get('a b'), 'c');
    }

    {
      let params =  new URLSearchParams('a=b%20c');
      strictEqual(params.get('a'), 'b c');
      params = new URLSearchParams('a%20b=c');
      strictEqual(params.get('a b'), 'c');
    };

    {
      let params =  new URLSearchParams('a=b\0c');
      strictEqual(params.get('a'), 'b\0c');
      params = new URLSearchParams('a\0b=c');
      strictEqual(params.get('a\0b'), 'c');
    }

    {
      let params =  new URLSearchParams('a=b%00c');
      strictEqual(params.get('a'), 'b\0c');
      params = new URLSearchParams('a%00b=c');
      strictEqual(params.get('a\0b'), 'c');
    }

    {
      let params =  new URLSearchParams('a=b\u2384');
      strictEqual(params.get('a'), 'b\u2384');
      params = new URLSearchParams('a\u2384b=c');
      strictEqual(params.get('a\u2384b'), 'c');
    }

    {
      let params =  new URLSearchParams('a=b%e2%8e%84');
      strictEqual(params.get('a'), 'b\u2384');
      params = new URLSearchParams('a%e2%8e%84b=c');
      strictEqual(params.get('a\u2384b'), 'c');
    }

    {
      let params =  new URLSearchParams('a=b\uD83D\uDCA9c');
      strictEqual(params.get('a'), 'b\uD83D\uDCA9c');
      params = new URLSearchParams('a\uD83D\uDCA9b=c');
      strictEqual(params.get('a\uD83D\uDCA9b'), 'c');
    }

    {
      let params =  new URLSearchParams('a=b%f0%9f%92%a9c');
      strictEqual(params.get('a'), 'b\uD83D\uDCA9c');
      params = new URLSearchParams('a%f0%9f%92%a9b=c');
      strictEqual(params.get('a\uD83D\uDCA9b'), 'c');
    }

    {
      let params =  new URLSearchParams([]);
      params = new URLSearchParams([['a', 'b'], ['c', 'd']]);
      strictEqual(params.get("a"), "b");
      strictEqual(params.get("c"), "d");
      throws(() => new URLSearchParams([[1]]));
      throws(() => new URLSearchParams([[1,2,3]]));
    }

    {
      let params = new URLSearchParams('a=a/b~');
      strictEqual(params.toString(), 'a=a%2Fb%7E');

      let url = new URL('https://example.org?a=a/b~');
      strictEqual(url.search, '?a=a/b~');
      strictEqual(url.searchParams.toString(), 'a=a%2Fb%7E');
      console.log(url.search);
      console.log(url.searchParams.toString());
    }

    [
      { "input": {"+": "%C2"}, "output": [["+", "%C2"]], "name": "object with +" },
      { "input": {c: "x", a: "?"}, "output": [["c", "x"], ["a", "?"]], "name": "object with two keys" },
      { "input": [["c", "x"], ["a", "?"]], "output": [["c", "x"], ["a", "?"]], "name": "array with two keys" },
    ].forEach((val) => {
      let params = new URLSearchParams(val.input);
      let i = 0;
      for (let param of params) {
        deepStrictEqual(param, val.output[i])
        i++
      }
    })

    {
      const params = new URLSearchParams()
      params[Symbol.iterator] = function *() {
        yield ["a", "b"]
      }
      let params2 = new URLSearchParams(params)
      strictEqual(params2.get("a"), "b")
    }
  }
};

export const w3cTestURLSearchParamsAppend = {
  test() {
    {
      const params = new URLSearchParams();
      params.append('a', 'b');
      strictEqual(params + '', 'a=b');
      params.append('a', 'b');
      strictEqual(params + '', 'a=b&a=b');
      params.append('a', 'c');
      strictEqual(params + '', 'a=b&a=b&a=c');
    }

    {
      const params = new URLSearchParams();
      params.append('', '');
      strictEqual(params + '', '=');
      params.append('', '');
      strictEqual(params + '', '=&=');
    }

    {
      const params = new URLSearchParams();
      params.append(null, null);
      strictEqual(params + '', 'null=null');
      params.append(null, null);
      strictEqual(params + '', 'null=null&null=null');
    }

    {
      const params = new URLSearchParams();
      params.append('first', 1);
      params.append('second', 2);
      params.append('third', '');
      params.append('first', 10);
      ok(params.has('first'));
      strictEqual(params.get('first'), '1');
      strictEqual(params.get('second'), '2');
      strictEqual(params.get('third'), '');
      params.append('first', 10);
      strictEqual(params.get('first'), '1');
    }
  }
};

export const w3cTestURLSearchParamsDelete = {
  test() {
    {
      let params = new URLSearchParams('a=b&c=d');
      params.delete('a');
      strictEqual(params + '', 'c=d');
      params = new URLSearchParams('a=a&b=b&a=a&c=c');
      params.delete('a');
      strictEqual(params + '', 'b=b&c=c');
      params = new URLSearchParams('a=a&=&b=b&c=c');
      params.delete('');
      strictEqual(params + '', 'a=a&b=b&c=c');
      params = new URLSearchParams('a=a&null=null&b=b');
      params.delete(null);
      strictEqual(params + '', 'a=a&b=b');
      params = new URLSearchParams('a=a&undefined=undefined&b=b');
      params.delete(undefined);
      strictEqual(params + '', 'a=a&b=b');
    }

    {
      const params = new URLSearchParams();
      params.append('first', 1);
      ok(params.has('first'));
      strictEqual(params.get('first'), '1');
      params.delete('first');
      ok(!params.has('first'));
      params.append('first', 1);
      params.append('first', 10);
      params.delete('first');
      ok(!params.has('first'));
    }

    {
      const url = new URL('http://example.com/?param1&param2');
      url.searchParams.delete('param1');
      url.searchParams.delete('param2');
      strictEqual(url.href, 'http://example.com/');
      strictEqual(url.search, '');
    }

    {
      const url = new URL('http://example.com/?');
      url.searchParams.delete('param1');
      strictEqual(url.href, 'http://example.com/');
      strictEqual(url.search, '');
    }
  }
};

export const w3cTestURLSearchParamsGet = {
  test() {
    {
      let params = new URLSearchParams('a=b&c=d');
      strictEqual(params.get('a'), 'b');
      strictEqual(params.get('c'), 'd');
      strictEqual(params.get('e'), null);
      params = new URLSearchParams('a=b&c=d&a=e');
      strictEqual(params.get('a'), 'b');
      params = new URLSearchParams('=b&c=d');
      strictEqual(params.get(''), 'b');
      params = new URLSearchParams('a=&c=d&a=e');
      strictEqual(params.get('a'), '');
    }

    {
      const params = new URLSearchParams('first=second&third&&');
      ok(params != null);
      ok(params.has('first'));
      strictEqual(params.get('first'), 'second');
      strictEqual(params.get('third'), '');
      strictEqual(params.get('fourth'), null);
    }
  }
};

export const w3cTestURLSearchParamsGetAll = {
  test() {
    {
      let params = new URLSearchParams('a=b&c=d');
      deepStrictEqual(params.getAll('a'), ['b']);
      deepStrictEqual(params.getAll('c'), ['d']);
      deepStrictEqual(params.getAll('e'), []);
      params = new URLSearchParams('a=b&c=d&a=e');
      deepStrictEqual(params.getAll('a'), ['b', 'e']);
      params = new URLSearchParams('=b&c=d');
      deepStrictEqual(params.getAll(''), ['b']);
      params = new URLSearchParams('a=&c=d&a=e');
      deepStrictEqual(params.getAll('a'), ['', 'e']);
    }

    {
      let params = new URLSearchParams('a=1&a=2&a=3&a');
      ok(params.has('a'));
      var matches = params.getAll('a');
      ok(matches && matches.length == 4, 'Search params object has values for name "a"');
      deepStrictEqual(matches, ['1', '2', '3', '']);
      params.set('a', 'one');
      strictEqual(params.get('a'), 'one');
      var matches = params.getAll('a');
      ok(matches && matches.length == 1);
      deepStrictEqual(matches, ['one']);
    }
  }
};

export const w3cTestURLSearchParamsHas = {
  test() {
    {
      let params = new URLSearchParams('a=b&c=d');
      ok(params.has('a'));
      ok(params.has('c'));
      ok(!params.has('e'));
      params = new URLSearchParams('a=b&c=d&a=e');
      ok(params.has('a'));
      params = new URLSearchParams('=b&c=d');
      ok(params.has(''));
      params = new URLSearchParams('null=a');
      ok(params.has(null));
    }

    {
      let params = new URLSearchParams('a=b&c=d&&');
      params.append('first', 1);
      params.append('first', 2);
      ok(params.has('a'));
      ok(params.has('c'));
      ok(params.has('first'));
      ok(!params.has('d'));
      params.delete('first');
      ok(!params.has('first'));
    }
  }
};

export const w3cTestURLSearchParamsSet = {
  test() {
     {
      let params = new URLSearchParams('a=b&c=d');
      params.set('a', 'B');
      strictEqual(params + '', 'a=B&c=d');
      params = new URLSearchParams('a=b&c=d&a=e');
      params.set('a', 'B');
      strictEqual(params + '', 'a=B&c=d')
      params.set('e', 'f');
      strictEqual(params + '', 'a=B&c=d&e=f')
    }

    {
      let params = new URLSearchParams('a=1&a=2&a=3');
      ok(params.has('a'));
      strictEqual(params.get('a'), '1');
      params.set('first', 4);
      ok(params.has('a'));
      strictEqual(params.get('a'), '1');
      params.set('a', 4);
      ok(params.has('a'));
      strictEqual(params.get('a'), '4');
    }
  }
};

export const w3cTestURLSearchParamsSort = {
  test() {
    [
      {
        "input": "z=b&a=b&z=a&a=a",
        "output": [["a", "b"], ["a", "a"], ["z", "b"], ["z", "a"]]
      },
      {
        "input": "\uFFFD=x&\uFFFC&\uFFFD=a",
        "output": [["\uFFFC", ""], ["\uFFFD", "x"], ["\uFFFD", "a"]]
      },
      {
        "input": "ﬃ&🌈", // 🌈 > code point, but < code unit because two code units
        "output": [["ﬃ", ""], ["🌈", ""]]
      },
      {
        "input": "é&e\uFFFD&e\u0301",
        "output": [["e\u0301", ""], ["e\uFFFD", ""], ["é", ""]]
      },
      {
        "input": "z=z&a=a&z=y&a=b&z=x&a=c&z=w&a=d&z=v&a=e&z=u&a=f&z=t&a=g",
        "output": [["a", "a"], ["a", "b"], ["a", "c"], ["a", "d"], ["a", "e"], ["a", "f"], ["a", "g"], ["z", "z"], ["z", "y"], ["z", "x"], ["z", "w"], ["z", "v"], ["z", "u"], ["z", "t"]]
      }
    ].forEach((val) => {
      {
        const params = new URLSearchParams(val.input);
        let i = 0;
        params.sort()
        for(let param of params) {
          deepStrictEqual(param, val.output[i])
          i++
        }
      }

      {
        const url = new URL("?" + val.input, "https://example/")
        url.searchParams.sort()
        const params = new URLSearchParams(url.search);
        let i = 0;
        for(let param of params) {
          deepStrictEqual(param, val.output[i]);
          i++;
        }
      }
    })

    {
      const url = new URL("http://example.com/?")
      url.searchParams.sort()
      strictEqual(url.href, "http://example.com/")
      strictEqual(url.search, "")
    }
  }
}

export const w3cTestURLSearchParamsStringifier = {
  test() {
    {
      const params = new URLSearchParams();
      params.append('a', 'b c');
      strictEqual(params + '', 'a=b+c');
      params.delete('a');
      params.append('a b', 'c');
      strictEqual(params + '', 'a+b=c');
    }

    {
      const params = new URLSearchParams();
      params.append('a', '');
      strictEqual(params + '', 'a=');
      params.append('a', '');
      strictEqual(params + '', 'a=&a=');
      params.append('', 'b');
      strictEqual(params + '', 'a=&a=&=b');
      params.append('', '');
      strictEqual(params + '', 'a=&a=&=b&=');
      params.append('', '');
      strictEqual(params + '', 'a=&a=&=b&=&=');
    }

    {
      const params = new URLSearchParams();
      params.append('', 'b');
      strictEqual(params + '', '=b');
      params.append('', 'b');
      strictEqual(params + '', '=b&=b');
    }

    {
      const params = new URLSearchParams();
      params.append('', '');
      strictEqual(params + '', '=');
      params.append('', '');
      strictEqual(params + '', '=&=');
    }

    {
      const params = new URLSearchParams();
      params.append('a', 'b+c');
      strictEqual(params + '', 'a=b%2Bc');
      params.delete('a');
      params.append('a+b', 'c');
      strictEqual(params + '', 'a%2Bb=c');
    }

    {
      const params = new URLSearchParams();
      params.append('=', 'a');
      strictEqual(params + '', '==a');
      params.append('b', '=');
      strictEqual(params + '', '==a&b==');
    }

    {
      const params = new URLSearchParams();
      params.append('&', 'a');
      strictEqual(params + '', '%26=a');
      params.append('b', '&');
      strictEqual(params + '', '%26=a&b=%26');
    }

    {
      const params = new URLSearchParams();
      params.append('a', '*-._');
      strictEqual(params + '', 'a=*-._');
      params.delete('a');
      params.append('*-._', 'c');
      strictEqual(params + '', '*-._=c');
    }

    {
      const params = new URLSearchParams();
      params.append('a', 'b%c');
      strictEqual(params + '', 'a=b%25c');
      params.delete('a');
      params.append('a%b', 'c');
      strictEqual(params + '', 'a%25b=c');
    }

    {
      const params = new URLSearchParams();
      params.append('a', 'b\0c');
      strictEqual(params + '', 'a=b%00c');
      params.delete('a');
      params.append('a\0b', 'c');
      strictEqual(params + '', 'a%00b=c');
    }

    {
      const params = new URLSearchParams();
      params.append('a', 'b\uD83D\uDCA9c');
      strictEqual(params + '', 'a=b%F0%9F%92%A9c');
      params.delete('a');
      params.append('a\uD83D\uDCA9b', 'c');
      strictEqual(params + '', 'a%F0%9F%92%A9b=c');
    }

    {
      let params = new URLSearchParams('a=b&c=d&&e&&');
      strictEqual(params.toString(), 'a=b&c=d&e=');
      params = new URLSearchParams('a = b &a=b&c=d%20');
      strictEqual(params.toString(), 'a+=+b+&a=b&c=d+');
      // The lone '=' _does_ survive the roundtrip.
      params = new URLSearchParams('a=&a=b');
      strictEqual(params.toString(), 'a=&a=b');
    }

    {
      const url = new URL('http://www.example.com/?a=b,c');
      const params = url.searchParams;

      strictEqual(url.toString(), 'http://www.example.com/?a=b,c');
      strictEqual(params.toString(), 'a=b%2Cc');

      params.append('x', 'y');

      strictEqual(url.toString(), 'http://www.example.com/?a=b%2Cc&x=y');
      strictEqual(params.toString(), 'a=b%2Cc&x=y');
    }
  }
};

export const urlSetSearch = {
  test() {
    const url = new URL("http://example.com?foo=bar&baz=qux");
    url.search = "?quux=corge&grault=garply";
    const result = [];
    for (let param of url.searchParams) {
      result.push(param.join(":"));
    }
    deepStrictEqual(result, [
      'quux:corge',
      'grault:garply'
    ]);
  }
};

export const urlSegfaultRegression = {
  test() {
    let url = new URL('http://example.org/?foo=bar&baz=qux');
    // Setting certain properties would once zero out an internal reference count, causing a
    // nondeterministic crash during URL/URLSearchParams destruction. It triggered an assertion
    // under debug mode when the `searchParams` property was accessed, which this test is designed
    // to express.
    url.protocol = "http:";
    strictEqual(url.searchParams.toString(), "foo=bar&baz=qux");
  }
};

// ======================================================================================

export const urlPatternBasics = {
  test() {
    const urlPattern = new URLPattern();

    // The standard attributes exist as they should and are readonly
    strictEqual(urlPattern.protocol, "*");
    strictEqual(urlPattern.username, "*");
    strictEqual(urlPattern.password, "*");
    strictEqual(urlPattern.hostname, "*");
    strictEqual(urlPattern.port, "*");
    strictEqual(urlPattern.pathname, "*");
    strictEqual(urlPattern.search, "*");
    strictEqual(urlPattern.hash, "*");

    strictEqual(typeof urlPattern.exec, 'function');
    strictEqual(typeof urlPattern.test, 'function');

    {
      // URLPattern can be correctly subclassed.
      let count = 0;
      class MyURLPattern extends URLPattern {
        get protocol() {
          count++;
          return super.protocol;
        }
        get username() {
          count++;
          return super.username;
        }
        get password() {
          count++;
          return super.password;
        }
        get hostname() {
          count++;
          return super.hostname;
        }
        get port() {
          count++;
          return super.port;
        }
        get pathname() {
          count++;
          return super.pathname;
        }
        get search() {
          count++;
          return super.search;
        }
        get hash() {
          count++;
          return super.hash;
        }
        test() { return true; }
        exec() { return true; }
      }

      const myPattern = new MyURLPattern();
      strictEqual(myPattern.protocol, "*");
      strictEqual(myPattern.username, "*");
      strictEqual(myPattern.password, "*");
      strictEqual(myPattern.hostname, "*");
      strictEqual(myPattern.port, "*");
      strictEqual(myPattern.pathname, "*");
      strictEqual(myPattern.search, "*");
      strictEqual(myPattern.hash, "*");
      strictEqual(myPattern.exec(), true);
      strictEqual(myPattern.test(), true);
      strictEqual(count, 8);
    }
  }
};

export const urlPatternMdn1 = {
  test() {
    const pattern = new URLPattern({ pathname: '/books' });
    ok(pattern.test('https://example.com/books'));

    const res = pattern.exec('https://example.com/books');
    strictEqual(res.inputs[0], 'https://example.com/books');
    strictEqual(res.protocol.input, 'https');
    strictEqual(res.protocol.groups["0"], "https");
    strictEqual(res.username.input, "");
    strictEqual(res.username.groups["0"], "");
    strictEqual(res.password.input, "");
    strictEqual(res.password.groups["0"], "");
    strictEqual(res.hostname.input, "example.com");
    strictEqual(res.hostname.groups["0"], "example.com");
    strictEqual(res.pathname.input, "/books");
    deepStrictEqual(Object.keys(res.pathname.groups), []);
    strictEqual(res.search.input, "");
    strictEqual(res.search.groups["0"], "");
    strictEqual(res.hash.input, "");
    strictEqual(res.hash.groups["0"], "");
  }
};

export const urlPatternMdn2 = {
  test() {
    const pattern = new URLPattern({ pathname: '/books/:id' });
    ok(pattern.test('https://example.com/books/123'));
    strictEqual(pattern.exec('https://example.com/books/123').pathname.groups.id, "123");
  }
};

export const urlPatternMdn3 = {
  test() {
    const pattern = new URLPattern('/books/:id(\\d+)', 'https://example.com');
    ok(pattern.test('https://example.com/books/123'));
    ok(!pattern.test('https://example.com/books/abc'));
    ok(!pattern.test('https://example.com/books/'));
  }
};

export const urlPatternMdn4 = {
  test() {
    const pattern = new URLPattern({ pathname: '/:type(foo|bar)' });
    const result = pattern.exec({ pathname: '/foo' });
    strictEqual(result.pathname.groups.type, 'foo');
  }
};

export const urlPatternMdn5 = {
  test() {
    const pattern = new URLPattern('/books/:id(\\d+)', 'https://example.com');
    strictEqual(pattern.exec('https://example.com/books/123').pathname.groups.id, "123");
  }
};

export const urlPatternMdn6 = {
  test() {
    const pattern = new URLPattern('/books/(\\d+)', 'https://example.com');
    strictEqual(pattern.exec('https://example.com/books/123').pathname.groups["0"], "123");
  }
};

export const urlPatternMdn7 = {
  test() {
    const pattern = new URLPattern('/books/:id?', 'https://example.com');
    ok(pattern.test('https://example.com/books/123'));
    ok(pattern.test('https://example.com/books'));
    ok(!pattern.test('https://example.com/books/'));
    ok(!pattern.test('https://example.com/books/123/456'));
    ok(!pattern.test('https://example.com/books/123/456/789'));
  }
};

export const urlPatternMdn8 = {
  test() {
    // A repeating group with a minimum of one
    const pattern = new URLPattern('/books/:id+', 'https://example.com');
    ok(pattern.test('https://example.com/books/123'));
    ok(!pattern.test('https://example.com/books'));
    ok(!pattern.test('https://example.com/books/'));
    ok(pattern.test('https://example.com/books/123/456'));
    ok(pattern.test('https://example.com/books/123/456/789'));
  }
};

export const urlPatternMdn9 = {
  test() {
    const pattern = new URLPattern('/books/:id*', 'https://example.com');
    ok(pattern.test('https://example.com/books/123'));
    ok(pattern.test('https://example.com/books'));
    ok(!pattern.test('https://example.com/books/'));
    ok(pattern.test('https://example.com/books/123/456'));
    ok(pattern.test('https://example.com/books/123/456/789'));
  }
};

export const urlPatternMdn10 = {
  test() {
    const pattern = new URLPattern('/book{s}?', 'https://example.com');
    ok(pattern.test('https://example.com/books'));
    ok(pattern.test('https://example.com/book'));
    deepStrictEqual(Object.keys(pattern.exec('https://example.com/books').pathname.groups), []);
  }
};

export const urlPatternMdn11 = {
  test() {
    // A group delimiter without a modifier
    const pattern = new URLPattern('/book{s}', 'https://example.com');
    strictEqual(pattern.pathname, '/books');
    ok(pattern.test('https://example.com/books'));
    ok(!pattern.test('https://example.com/book'));
  }
};

export const urlPatternMdn12 = {
  test() {
    // A group delimiter containing a capturing group
    const pattern = new URLPattern({ pathname: '/blog/:id(\\d+){-:title}?' });
    ok(pattern.test('https://example.com/blog/123-my-blog'));
    ok(pattern.test('https://example.com/blog/123'));
    ok(!pattern.test('https://example.com/blog/my-blog'));
  }
};

export const urlPatternMdn13 = {
  test() {
    const pattern = new URLPattern('/books/:id?', 'https://example.com');
    ok(pattern.test('https://example.com/books/123'));
    ok(pattern.test('https://example.com/books'));
    ok(!pattern.test('https://example.com/books/'));
  }
};

export const urlPatternMdn14 = {
  test() {
    const pattern = new URLPattern('/books/:id+', 'https://example.com');
    ok(pattern.test('https://example.com/books/123'));
    ok(pattern.test('https://example.com/books/123/456'));
    ok(!pattern.test('https://example.com/books/123/'));
    ok(!pattern.test('https://example.com/books/123/456/'));
  }
};

export const urlPatternMdn15 = {
  test() {
    const pattern = new URLPattern({ hash: '/books/:id?' });
    ok(pattern.test('https://example.com#/books/123'));
    ok(!pattern.test('https://example.com#/books'));
    ok(pattern.test('https://example.com#/books/'));
  }
};

export const urlPatternMdn16 = {
  test() {
    const pattern = new URLPattern({ pathname: '/books/{:id}?' });
    ok(pattern.test('https://example.com/books/123'));
    ok(!pattern.test('https://example.com/books'));
    ok(pattern.test('https://example.com/books/'));
  }
};

export const urlPatternMdn17 = {
  test() {
    const pattern = new URLPattern('/books/*', 'https://example.com');
    ok(pattern.test('https://example.com/books/123'));
    ok(!pattern.test('https://example.com/books'));
    ok(pattern.test('https://example.com/books/'));
    ok(pattern.test('https://example.com/books/123/456'));
  }
};

export const urlPatternMdn18 = {
  test() {
    const pattern = new URLPattern('/*.png', 'https://example.com');
    ok(pattern.test('https://example.com/image.png'));
    ok(!pattern.test('https://example.com/image.png/123'));
    ok(pattern.test('https://example.com/folder/image.png'));
    ok(pattern.test('https://example.com/.png'));
  }
};

export const urlPatternMdn19 = {
  test() {
    // Construct a URLPattern that matches a specific domain and its subdomains.
    // All other URL components default to the wildcard `*` pattern.
    const pattern = new URLPattern({
      hostname: '{*.}?example.com',
    });
    strictEqual(pattern.hostname, "{*.}?example.com");
    strictEqual(pattern.protocol, "*");
    strictEqual(pattern.username, "*");
    strictEqual(pattern.password, "*");
    strictEqual(pattern.pathname, "*");
    strictEqual(pattern.search, "*");
    strictEqual(pattern.hash, "*");
    ok(pattern.test('https://example.com/foo/bar'));
    ok(pattern.test({ hostname: 'cdn.example.com' }));
    ok(pattern.test('custom-protocol://example.com/other/path?q=1'));
    ok(!pattern.test('https://cdn-example.com/foo/bar'));
  }
};

export const urlPatternMdn20 = {
  test() {
    // Construct a URLPattern that matches URLs to CDN servers loading jpg images.
    // URL components not explicitly specified, like search and hash here, result
    // in the empty string similar to the URL() constructor.
    const pattern = new URLPattern('https://cdn-*.example.com/*.jpg');
    strictEqual(pattern.protocol, 'https');
    strictEqual(pattern.hostname, 'cdn-*.example.com');
    strictEqual(pattern.pathname, '/*.jpg');
    strictEqual(pattern.username, '');
    strictEqual(pattern.password, '');
    strictEqual(pattern.search, "");
    strictEqual(pattern.hash, "");
    ok(pattern.test("https://cdn-1234.example.com/product/assets/hero.jpg"));
    ok(!pattern.test("https://cdn-1234.example.com/product/assets/hero.jpg?q=1"));
  }
};

export const urlPatternMdn21 = {
  test() {
    throws(() => new URLPattern('data:foo*'));
  }
};

export const urlPatternMdn22 = {
  test() {
    const pattern = new URLPattern({ hostname: 'example.com', pathname: '/foo/*' });
    ok(pattern.test({
      pathname: '/foo/bar',
      baseURL: 'https://example.com/baz',
    }));
    ok(pattern.test('/foo/bar', 'https://example.com/baz'));
    // Throws because the second argument cannot be passed with a dictionary input.
    throws(() => {
      pattern.test({ pathname: '/foo/bar' }, 'https://example.com/baz');
    });
    // The `exec()` method takes the same arguments as `test()`.
    const result = pattern.exec('/foo/bar', 'https://example.com/baz');
    strictEqual(result.pathname.input, '/foo/bar');
    strictEqual(result.pathname.groups[0], 'bar');
    strictEqual(result.hostname.input, 'example.com');
  }
};

export const urlPatternMdn23 = {
  test() {
    const pattern1 = new URLPattern({ pathname: '/foo/*',
                                      baseURL: 'https://example.com' });
    strictEqual(pattern1.protocol, 'https');
    strictEqual(pattern1.hostname, 'example.com');
    strictEqual(pattern1.pathname, '/foo/*');
    strictEqual(pattern1.username, '');
    strictEqual(pattern1.password, '');
    strictEqual(pattern1.port, '');
    strictEqual(pattern1.search, '');
    strictEqual(pattern1.hash, '');
    new URLPattern('/foo/*', 'https://example.com');
    throws(() => new URLPattern('/foo/*'));
  }
};

export const urlPatternMdn24 = {
  test() {
    const pattern = new URLPattern({ hostname: '*.example.com' });
    const result = pattern.exec({ hostname: 'cdn.example.com' });
    strictEqual(result.hostname.groups[0], 'cdn');
    strictEqual(result.hostname.input, 'cdn.example.com');
    strictEqual(result.inputs?.[0].hostname, 'cdn.example.com');
  }
};

export const urlPatternMdn25 = {
  test() {
    // Construct a URLPattern using matching groups with custom names.  These
    // names can then be later used to access the matched values in the result
    // object.
    const pattern = new URLPattern({ pathname: '/:product/:user/:action' });
    const result = pattern.exec({ pathname: '/store/wanderview/view' });
    strictEqual(result.pathname.groups.product, 'store');
    strictEqual(result.pathname.groups.user, 'wanderview');
    strictEqual(result.pathname.groups.action, 'view');
    strictEqual(result.pathname.input, '/store/wanderview/view');
    strictEqual(result.inputs?.[0].pathname, '/store/wanderview/view');
  }
};

export const urlPattern26 = {
  test() {
    const pattern = new URLPattern({ pathname: '/product/:action+' });
    const result = pattern.exec({ pathname: '/product/do/some/thing/cool' });
    strictEqual(result.pathname.groups.action, 'do/some/thing/cool');
    ok(!pattern.test({ pathname: '/product' }));
  }
};

export const urlPattern27 = {
  test() {
    const pattern = new URLPattern({ pathname: '/product/:action*' });
    const result = pattern.exec({ pathname: '/product/do/some/thing/cool' });
    strictEqual(result.pathname.groups.action, 'do/some/thing/cool');
    ok(pattern.test({ pathname: '/product' }));
  }
};

export const urlPattern28 = {
  test() {
    const pattern = new URLPattern({ hostname: '{:subdomain.}*example.com' });
    ok(pattern.test({ hostname: 'example.com' }));
    ok(pattern.test({ hostname: 'foo.bar.example.com' }));
    ok(!pattern.test({ hostname: '.example.com' }));
    const result = pattern.exec({ hostname: 'foo.bar.example.com' });
    strictEqual(result.hostname.groups.subdomain, 'foo.bar');
  }
};

export const urlPattern29 = {
  test() {
    const pattern = new URLPattern({ pathname: '/product{/}?' });
    ok(pattern.test({ pathname: '/product' }));
    ok(pattern.test({ pathname: '/product/' }));
    const result = pattern.exec({ pathname: '/product/' });
    deepStrictEqual(Object.keys(result.pathname.groups), []);
  }
};

export const urlPattern32 = {
  test() {
    const pattern = new URLPattern({
      protocol: 'http{s}?',
      username: ':user?',
      password: ':pass?',
      hostname: '{:subdomain.}*example.com',
      pathname: '/product/:action*',
    });

    const result = pattern.exec(
      'http://foo:bar@sub.example.com/product/view?q=12345',
    );

    strictEqual(result.username.groups.user, 'foo');
    strictEqual(result.password.groups.pass, 'bar');
    strictEqual(result.hostname.groups.subdomain, 'sub');
    strictEqual(result.pathname.groups.action, 'view');
  }
};

export const urlPattern33 = {
  test() {
    // Constructs a URLPattern treating the `:` as the protocol suffix.
    const pattern = new URLPattern('data\\:foo*');
    strictEqual(pattern.protocol, 'data');
    strictEqual(pattern.pathname, 'foo*');
    strictEqual(pattern.username, '');
    strictEqual(pattern.password, '');
    strictEqual(pattern.hostname, '');
    strictEqual(pattern.port, '');
    strictEqual(pattern.search, '');
    strictEqual(pattern.hash, '');
    ok(pattern.test('data:foobar'));
  }
};

export const urlPattern34 = {
  test() {
    const pattern = new URLPattern({ pathname: '/(foo|bar)' });
    ok(pattern.test({ pathname: '/foo' }));
    ok(pattern.test({ pathname: '/bar' }));
    ok(!pattern.test({ pathname: '/baz' }));
    const result = pattern.exec({ pathname: '/foo' });
    strictEqual(result.pathname.groups[0], 'foo');
  }
};

export const urlPatternMdn35 = {
  test() {
    const pattern = new URLPattern({ pathname: '/product/(index.html)?' });
    ok(pattern.test({ pathname: '/product/index.html' }));
    ok(pattern.test({ pathname: '/product' }));
    const pattern2 = new URLPattern({ pathname: '/product/:action?' });
    ok(pattern2.test({ pathname: '/product/view' }));
    ok(pattern2.test({ pathname: '/product' }));
    // Wildcards can be made optional as well.  This may not seem to make sense
    // since they already match the empty string, but it also makes the prefix
    // `/` optional in a pathname pattern.
    const pattern3 = new URLPattern({ pathname: '/product/*?' });
    ok(pattern3.test({ pathname: '/product/wanderview/view' }));
    ok(pattern3.test({ pathname: '/product' }));
    ok(pattern3.test({ pathname: '/product/' }));
  }
};

export const urlPatternFun = {
  test() {
    // The pattern is not useful but it should parse and produce the
    // same results we see in the browser.
    const pattern = new URLPattern(":café://:\u1234/:_✔️");
    strictEqual(pattern.protocol, ":café")
    strictEqual(pattern.hostname, ":ሴ");
    strictEqual(pattern.pathname, "/:_%E2%9C%94%EF%B8%8F");

    {
      // There was a bug that would cause a crash. Instead the following should throw
      // as invalid URLPattern syntax.
      throws(() => new URLPattern({ hash: "=((" }));
    }
  }
};

export const urlParseStatic = {
  test() {
    const url = URL.parse('http://example.org');
    strictEqual(url.protocol, 'http:');
    strictEqual(url.host, 'example.org');

    const url2 = URL.parse('foo', 'http://example.org');
    strictEqual(url2.protocol, 'http:');
    strictEqual(url2.host, 'example.org');
    strictEqual(url2.pathname, '/foo');
  }
};
