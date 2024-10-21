import url from 'node:url';
import assert from 'node:assert';
import { inspect } from 'node:util';

// Ref: https://github.com/nodejs/node/blob/e92446536ed4e268c9eef6ae6f911e384c98eecf/test/parallel/test-url-format.js
export const format = {
  async test() {
    const formatTests = {
      'http://example.com?': {
        href: 'http://example.com/?',
        protocol: 'http:',
        slashes: true,
        host: 'example.com',
        hostname: 'example.com',
        search: '?',
        query: {},
        pathname: '/',
      },
      'http://example.com?foo=bar#frag': {
        href: 'http://example.com/?foo=bar#frag',
        protocol: 'http:',
        host: 'example.com',
        hostname: 'example.com',
        hash: '#frag',
        search: '?foo=bar',
        query: 'foo=bar',
        pathname: '/',
      },
      'http://example.com?foo=@bar#frag': {
        href: 'http://example.com/?foo=@bar#frag',
        protocol: 'http:',
        host: 'example.com',
        hostname: 'example.com',
        hash: '#frag',
        search: '?foo=@bar',
        query: 'foo=@bar',
        pathname: '/',
      },
      'http://example.com?foo=/bar/#frag': {
        href: 'http://example.com/?foo=/bar/#frag',
        protocol: 'http:',
        host: 'example.com',
        hostname: 'example.com',
        hash: '#frag',
        search: '?foo=/bar/',
        query: 'foo=/bar/',
        pathname: '/',
      },
      'http://example.com?foo=?bar/#frag': {
        href: 'http://example.com/?foo=?bar/#frag',
        protocol: 'http:',
        host: 'example.com',
        hostname: 'example.com',
        hash: '#frag',
        search: '?foo=?bar/',
        query: 'foo=?bar/',
        pathname: '/',
      },
      'http://example.com#frag=?bar/#frag': {
        href: 'http://example.com/#frag=?bar/#frag',
        protocol: 'http:',
        host: 'example.com',
        hostname: 'example.com',
        hash: '#frag=?bar/#frag',
        pathname: '/',
      },
      'http://google.com" onload="alert(42)/': {
        href: 'http://google.com/%22%20onload=%22alert(42)/',
        protocol: 'http:',
        host: 'google.com',
        pathname: '/%22%20onload=%22alert(42)/',
      },
      'http://a.com/a/b/c?s#h': {
        href: 'http://a.com/a/b/c?s#h',
        protocol: 'http',
        host: 'a.com',
        pathname: 'a/b/c',
        hash: 'h',
        search: 's',
      },
      'xmpp:isaacschlueter@jabber.org': {
        href: 'xmpp:isaacschlueter@jabber.org',
        protocol: 'xmpp:',
        host: 'jabber.org',
        auth: 'isaacschlueter',
        hostname: 'jabber.org',
      },
      'http://atpass:foo%40bar@127.0.0.1/': {
        href: 'http://atpass:foo%40bar@127.0.0.1/',
        auth: 'atpass:foo@bar',
        hostname: '127.0.0.1',
        protocol: 'http:',
        pathname: '/',
      },
      'http://atslash%2F%40:%2F%40@foo/': {
        href: 'http://atslash%2F%40:%2F%40@foo/',
        auth: 'atslash/@:/@',
        hostname: 'foo',
        protocol: 'http:',
        pathname: '/',
      },
      'svn+ssh://foo/bar': {
        href: 'svn+ssh://foo/bar',
        hostname: 'foo',
        protocol: 'svn+ssh:',
        pathname: '/bar',
        slashes: true,
      },
      'dash-test://foo/bar': {
        href: 'dash-test://foo/bar',
        hostname: 'foo',
        protocol: 'dash-test:',
        pathname: '/bar',
        slashes: true,
      },
      'dash-test:foo/bar': {
        href: 'dash-test:foo/bar',
        hostname: 'foo',
        protocol: 'dash-test:',
        pathname: '/bar',
      },
      'dot.test://foo/bar': {
        href: 'dot.test://foo/bar',
        hostname: 'foo',
        protocol: 'dot.test:',
        pathname: '/bar',
        slashes: true,
      },
      'dot.test:foo/bar': {
        href: 'dot.test:foo/bar',
        hostname: 'foo',
        protocol: 'dot.test:',
        pathname: '/bar',
      },
      // IPv6 support
      'coap:u:p@[::1]:61616/.well-known/r?n=Temperature': {
        href: 'coap:u:p@[::1]:61616/.well-known/r?n=Temperature',
        protocol: 'coap:',
        auth: 'u:p',
        hostname: '::1',
        port: '61616',
        pathname: '/.well-known/r',
        search: 'n=Temperature',
      },
      'coap:[fedc:ba98:7654:3210:fedc:ba98:7654:3210]:61616/s/stopButton': {
        href: 'coap:[fedc:ba98:7654:3210:fedc:ba98:7654:3210]:61616/s/stopButton',
        protocol: 'coap',
        host: '[fedc:ba98:7654:3210:fedc:ba98:7654:3210]:61616',
        pathname: '/s/stopButton',
      },
      'http://[::]/': {
        href: 'http://[::]/',
        protocol: 'http:',
        hostname: '[::]',
        pathname: '/',
      },

      // Encode context-specific delimiters in path and query, but do not touch
      // other non-delimiter chars like `%`.
      // <https://github.com/nodejs/node-v0.x-archive/issues/4082>

      // `#`,`?` in path
      '/path/to/%%23%3F+=&.txt?foo=theA1#bar': {
        href: '/path/to/%%23%3F+=&.txt?foo=theA1#bar',
        pathname: '/path/to/%#?+=&.txt',
        query: {
          foo: 'theA1',
        },
        hash: '#bar',
      },

      // `#`,`?` in path + `#` in query
      '/path/to/%%23%3F+=&.txt?foo=the%231#bar': {
        href: '/path/to/%%23%3F+=&.txt?foo=the%231#bar',
        pathname: '/path/to/%#?+=&.txt',
        query: {
          foo: 'the#1',
        },
        hash: '#bar',
      },

      // `#` in path end + `#` in query
      '/path/to/%%23?foo=the%231#bar': {
        href: '/path/to/%%23?foo=the%231#bar',
        pathname: '/path/to/%#',
        query: {
          foo: 'the#1',
        },
        hash: '#bar',
      },

      // `?` and `#` in path and search
      'http://ex.com/foo%3F100%m%23r?abc=the%231?&foo=bar#frag': {
        href: 'http://ex.com/foo%3F100%m%23r?abc=the%231?&foo=bar#frag',
        protocol: 'http:',
        hostname: 'ex.com',
        hash: '#frag',
        search: '?abc=the#1?&foo=bar',
        pathname: '/foo?100%m#r',
      },

      // `?` and `#` in search only
      'http://ex.com/fooA100%mBr?abc=the%231?&foo=bar#frag': {
        href: 'http://ex.com/fooA100%mBr?abc=the%231?&foo=bar#frag',
        protocol: 'http:',
        hostname: 'ex.com',
        hash: '#frag',
        search: '?abc=the#1?&foo=bar',
        pathname: '/fooA100%mBr',
      },

      // Multiple `#` in search
      'http://example.com/?foo=bar%231%232%233&abc=%234%23%235#frag': {
        href: 'http://example.com/?foo=bar%231%232%233&abc=%234%23%235#frag',
        protocol: 'http:',
        slashes: true,
        host: 'example.com',
        hostname: 'example.com',
        hash: '#frag',
        search: '?foo=bar#1#2#3&abc=#4##5',
        query: {},
        pathname: '/',
      },

      // More than 255 characters in hostname which exceeds the limit
      [`http://${'a'.repeat(255)}.com/node`]: {
        href: 'http:///node',
        protocol: 'http:',
        slashes: true,
        host: '',
        hostname: '',
        pathname: '/node',
        path: '/node',
      },

      // Greater than or equal to 63 characters after `.` in hostname
      [`http://www.${'z'.repeat(63)}example.com/node`]: {
        href: `http://www.${'z'.repeat(63)}example.com/node`,
        protocol: 'http:',
        slashes: true,
        host: `www.${'z'.repeat(63)}example.com`,
        hostname: `www.${'z'.repeat(63)}example.com`,
        pathname: '/node',
        path: '/node',
      },

      // https://github.com/nodejs/node/issues/3361
      'file:///home/user': {
        href: 'file:///home/user',
        protocol: 'file',
        pathname: '/home/user',
        path: '/home/user',
      },

      // surrogate in auth
      'http://%F0%9F%98%80@www.example.com/': {
        href: 'http://%F0%9F%98%80@www.example.com/',
        protocol: 'http:',
        auth: '\uD83D\uDE00',
        hostname: 'www.example.com',
        pathname: '/',
      },
    };

    for (const u in formatTests) {
      const expect = formatTests[u].href;
      delete formatTests[u].href;
      const actual = url.format(u);
      const actualObj = url.format(formatTests[u]);
      assert.strictEqual(
        actual,
        expect,
        `wonky format(${u}) == ${expect}\nactual:${actual}`
      );
      assert.strictEqual(
        actualObj,
        expect,
        `wonky format(${JSON.stringify(formatTests[u])}) == ${
          expect
        }\nactual: ${actualObj}`
      );
    }
  },
};

// Ref: https://github.com/nodejs/node/blob/e92446536ed4e268c9eef6ae6f911e384c98eecf/test/parallel/test-url-relative.js
export const relative = {
  async test() {
    // When source is false
    assert.strictEqual(url.resolveObject('', 'foo'), 'foo');

    // [from, path, expected]
    const relativeTests = [
      ['/foo/bar/baz', 'quux', '/foo/bar/quux'],
      ['/foo/bar/baz', 'quux/asdf', '/foo/bar/quux/asdf'],
      ['/foo/bar/baz', 'quux/baz', '/foo/bar/quux/baz'],
      ['/foo/bar/baz', '../quux/baz', '/foo/quux/baz'],
      ['/foo/bar/baz', '/bar', '/bar'],
      ['/foo/bar/baz/', 'quux', '/foo/bar/baz/quux'],
      ['/foo/bar/baz/', 'quux/baz', '/foo/bar/baz/quux/baz'],
      ['/foo/bar/baz', '../../../../../../../../quux/baz', '/quux/baz'],
      ['/foo/bar/baz', '../../../../../../../quux/baz', '/quux/baz'],
      ['/foo', '.', '/'],
      ['/foo', '..', '/'],
      ['/foo/', '.', '/foo/'],
      ['/foo/', '..', '/'],
      ['/foo/bar', '.', '/foo/'],
      ['/foo/bar', '..', '/'],
      ['/foo/bar/', '.', '/foo/bar/'],
      ['/foo/bar/', '..', '/foo/'],
      ['foo/bar', '../../../baz', '../../baz'],
      ['foo/bar/', '../../../baz', '../baz'],
      [
        'http://example.com/b//c//d;p?q#blarg',
        'https:#hash2',
        'https:///#hash2',
      ],
      [
        'http://example.com/b//c//d;p?q#blarg',
        'https:/p/a/t/h?s#hash2',
        'https://p/a/t/h?s#hash2',
      ],
      [
        'http://example.com/b//c//d;p?q#blarg',
        'https://u:p@h.com/p/a/t/h?s#hash2',
        'https://u:p@h.com/p/a/t/h?s#hash2',
      ],
      [
        'http://example.com/b//c//d;p?q#blarg',
        'https:/a/b/c/d',
        'https://a/b/c/d',
      ],
      [
        'http://example.com/b//c//d;p?q#blarg',
        'http:#hash2',
        'http://example.com/b//c//d;p?q#hash2',
      ],
      [
        'http://example.com/b//c//d;p?q#blarg',
        'http:/p/a/t/h?s#hash2',
        'http://example.com/p/a/t/h?s#hash2',
      ],
      [
        'http://example.com/b//c//d;p?q#blarg',
        'http://u:p@h.com/p/a/t/h?s#hash2',
        'http://u:p@h.com/p/a/t/h?s#hash2',
      ],
      [
        'http://example.com/b//c//d;p?q#blarg',
        'http:/a/b/c/d',
        'http://example.com/a/b/c/d',
      ],
      ['/foo/bar/baz', '/../etc/passwd', '/etc/passwd'],
      ['http://localhost', 'file:///Users/foo', 'file:///Users/foo'],
      ['http://localhost', 'file://foo/Users', 'file://foo/Users'],
      [
        'https://registry.npmjs.org',
        '@foo/bar',
        'https://registry.npmjs.org/@foo/bar',
      ],
    ];
    for (let i = 0; i < relativeTests.length; i++) {
      const relativeTest = relativeTests[i];

      const a = url.resolve(relativeTest[0], relativeTest[1]);
      const e = relativeTest[2];
      assert.strictEqual(
        a,
        e,
        `resolve(${relativeTest[0]}, ${relativeTest[1]})` +
          ` == ${e}\n  actual=${a}`
      );
    }

    //
    // Tests below taken from Chiron
    // http://code.google.com/p/chironjs/source/browse/trunk/src/test/http/url.js
    //
    // Copyright (c) 2002-2008 Kris Kowal <http://cixar.com/~kris.kowal>
    // used with permission under MIT License
    //
    // Changes marked with @isaacs

    const bases = [
      'http://a/b/c/d;p?q',
      'http://a/b/c/d;p?q=1/2',
      'http://a/b/c/d;p=1/2?q',
      'fred:///s//a/b/c',
      'http:///s//a/b/c',
    ];

    // [to, from, result]
    const relativeTests2 = [
      // http://lists.w3.org/Archives/Public/uri/2004Feb/0114.html
      ['../c', 'foo:a/b', 'foo:c'],
      ['foo:.', 'foo:a', 'foo:'],
      ['/foo/../../../bar', 'zz:abc', 'zz:/bar'],
      ['/foo/../bar', 'zz:abc', 'zz:/bar'],
      // @isaacs Disagree. Not how web browsers resolve this.
      ['foo/../../../bar', 'zz:abc', 'zz:bar'],
      // ['foo/../../../bar',  'zz:abc', 'zz:../../bar'], // @isaacs Added
      ['foo/../bar', 'zz:abc', 'zz:bar'],
      ['zz:.', 'zz:abc', 'zz:'],
      ['/.', bases[0], 'http://a/'],
      ['/.foo', bases[0], 'http://a/.foo'],
      ['.foo', bases[0], 'http://a/b/c/.foo'],

      // http://gbiv.com/protocols/uri/test/rel_examples1.html
      // examples from RFC 2396
      ['g:h', bases[0], 'g:h'],
      ['g', bases[0], 'http://a/b/c/g'],
      ['./g', bases[0], 'http://a/b/c/g'],
      ['g/', bases[0], 'http://a/b/c/g/'],
      ['/g', bases[0], 'http://a/g'],
      ['//g', bases[0], 'http://g/'],
      // Changed with RFC 2396bis
      // ('?y', bases[0], 'http://a/b/c/d;p?y'],
      ['?y', bases[0], 'http://a/b/c/d;p?y'],
      ['g?y', bases[0], 'http://a/b/c/g?y'],
      // Changed with RFC 2396bis
      // ('#s', bases[0], CURRENT_DOC_URI + '#s'],
      ['#s', bases[0], 'http://a/b/c/d;p?q#s'],
      ['g#s', bases[0], 'http://a/b/c/g#s'],
      ['g?y#s', bases[0], 'http://a/b/c/g?y#s'],
      [';x', bases[0], 'http://a/b/c/;x'],
      ['g;x', bases[0], 'http://a/b/c/g;x'],
      ['g;x?y#s', bases[0], 'http://a/b/c/g;x?y#s'],
      // Changed with RFC 2396bis
      // ('', bases[0], CURRENT_DOC_URI],
      ['', bases[0], 'http://a/b/c/d;p?q'],
      ['.', bases[0], 'http://a/b/c/'],
      ['./', bases[0], 'http://a/b/c/'],
      ['..', bases[0], 'http://a/b/'],
      ['../', bases[0], 'http://a/b/'],
      ['../g', bases[0], 'http://a/b/g'],
      ['../..', bases[0], 'http://a/'],
      ['../../', bases[0], 'http://a/'],
      ['../../g', bases[0], 'http://a/g'],
      ['../../../g', bases[0], ('http://a/../g', 'http://a/g')],
      ['../../../../g', bases[0], ('http://a/../../g', 'http://a/g')],
      // Changed with RFC 2396bis
      // ('/./g', bases[0], 'http://a/./g'],
      ['/./g', bases[0], 'http://a/g'],
      // Changed with RFC 2396bis
      // ('/../g', bases[0], 'http://a/../g'],
      ['/../g', bases[0], 'http://a/g'],
      ['g.', bases[0], 'http://a/b/c/g.'],
      ['.g', bases[0], 'http://a/b/c/.g'],
      ['g..', bases[0], 'http://a/b/c/g..'],
      ['..g', bases[0], 'http://a/b/c/..g'],
      ['./../g', bases[0], 'http://a/b/g'],
      ['./g/.', bases[0], 'http://a/b/c/g/'],
      ['g/./h', bases[0], 'http://a/b/c/g/h'],
      ['g/../h', bases[0], 'http://a/b/c/h'],
      ['g;x=1/./y', bases[0], 'http://a/b/c/g;x=1/y'],
      ['g;x=1/../y', bases[0], 'http://a/b/c/y'],
      ['g?y/./x', bases[0], 'http://a/b/c/g?y/./x'],
      ['g?y/../x', bases[0], 'http://a/b/c/g?y/../x'],
      ['g#s/./x', bases[0], 'http://a/b/c/g#s/./x'],
      ['g#s/../x', bases[0], 'http://a/b/c/g#s/../x'],
      ['http:g', bases[0], ('http:g', 'http://a/b/c/g')],
      ['http:', bases[0], ('http:', bases[0])],
      // Not sure where this one originated
      ['/a/b/c/./../../g', bases[0], 'http://a/a/g'],

      // http://gbiv.com/protocols/uri/test/rel_examples2.html
      // slashes in base URI's query args
      ['g', bases[1], 'http://a/b/c/g'],
      ['./g', bases[1], 'http://a/b/c/g'],
      ['g/', bases[1], 'http://a/b/c/g/'],
      ['/g', bases[1], 'http://a/g'],
      ['//g', bases[1], 'http://g/'],
      // Changed in RFC 2396bis
      // ('?y', bases[1], 'http://a/b/c/?y'],
      ['?y', bases[1], 'http://a/b/c/d;p?y'],
      ['g?y', bases[1], 'http://a/b/c/g?y'],
      ['g?y/./x', bases[1], 'http://a/b/c/g?y/./x'],
      ['g?y/../x', bases[1], 'http://a/b/c/g?y/../x'],
      ['g#s', bases[1], 'http://a/b/c/g#s'],
      ['g#s/./x', bases[1], 'http://a/b/c/g#s/./x'],
      ['g#s/../x', bases[1], 'http://a/b/c/g#s/../x'],
      ['./', bases[1], 'http://a/b/c/'],
      ['../', bases[1], 'http://a/b/'],
      ['../g', bases[1], 'http://a/b/g'],
      ['../../', bases[1], 'http://a/'],
      ['../../g', bases[1], 'http://a/g'],

      // http://gbiv.com/protocols/uri/test/rel_examples3.html
      // slashes in path params
      // all of these changed in RFC 2396bis
      ['g', bases[2], 'http://a/b/c/d;p=1/g'],
      ['./g', bases[2], 'http://a/b/c/d;p=1/g'],
      ['g/', bases[2], 'http://a/b/c/d;p=1/g/'],
      ['g?y', bases[2], 'http://a/b/c/d;p=1/g?y'],
      [';x', bases[2], 'http://a/b/c/d;p=1/;x'],
      ['g;x', bases[2], 'http://a/b/c/d;p=1/g;x'],
      ['g;x=1/./y', bases[2], 'http://a/b/c/d;p=1/g;x=1/y'],
      ['g;x=1/../y', bases[2], 'http://a/b/c/d;p=1/y'],
      ['./', bases[2], 'http://a/b/c/d;p=1/'],
      ['../', bases[2], 'http://a/b/c/'],
      ['../g', bases[2], 'http://a/b/c/g'],
      ['../../', bases[2], 'http://a/b/'],
      ['../../g', bases[2], 'http://a/b/g'],

      // http://gbiv.com/protocols/uri/test/rel_examples4.html
      // double and triple slash, unknown scheme
      ['g:h', bases[3], 'g:h'],
      ['g', bases[3], 'fred:///s//a/b/g'],
      ['./g', bases[3], 'fred:///s//a/b/g'],
      ['g/', bases[3], 'fred:///s//a/b/g/'],
      ['/g', bases[3], 'fred:///g'], // May change to fred:///s//a/g
      ['//g', bases[3], 'fred://g'], // May change to fred:///s//g
      ['//g/x', bases[3], 'fred://g/x'], // May change to fred:///s//g/x
      ['///g', bases[3], 'fred:///g'],
      ['./', bases[3], 'fred:///s//a/b/'],
      ['../', bases[3], 'fred:///s//a/'],
      ['../g', bases[3], 'fred:///s//a/g'],

      ['../../', bases[3], 'fred:///s//'],
      ['../../g', bases[3], 'fred:///s//g'],
      ['../../../g', bases[3], 'fred:///s/g'],
      // May change to fred:///s//a/../../../g
      ['../../../../g', bases[3], 'fred:///g'],

      // http://gbiv.com/protocols/uri/test/rel_examples5.html
      // double and triple slash, well-known scheme
      ['g:h', bases[4], 'g:h'],
      ['g', bases[4], 'http:///s//a/b/g'],
      ['./g', bases[4], 'http:///s//a/b/g'],
      ['g/', bases[4], 'http:///s//a/b/g/'],
      ['/g', bases[4], 'http:///g'], // May change to http:///s//a/g
      ['//g', bases[4], 'http://g/'], // May change to http:///s//g
      ['//g/x', bases[4], 'http://g/x'], // May change to http:///s//g/x
      ['///g', bases[4], 'http:///g'],
      ['./', bases[4], 'http:///s//a/b/'],
      ['../', bases[4], 'http:///s//a/'],
      ['../g', bases[4], 'http:///s//a/g'],
      ['../../', bases[4], 'http:///s//'],
      ['../../g', bases[4], 'http:///s//g'],
      // May change to http:///s//a/../../g
      ['../../../g', bases[4], 'http:///s/g'],
      // May change to http:///s//a/../../../g
      ['../../../../g', bases[4], 'http:///g'],

      // From Dan Connelly's tests in http://www.w3.org/2000/10/swap/uripath.py
      ['bar:abc', 'foo:xyz', 'bar:abc'],
      ['../abc', 'http://example/x/y/z', 'http://example/x/abc'],
      ['http://example/x/abc', 'http://example2/x/y/z', 'http://example/x/abc'],
      ['../r', 'http://ex/x/y/z', 'http://ex/x/r'],
      ['q/r', 'http://ex/x/y', 'http://ex/x/q/r'],
      ['q/r#s', 'http://ex/x/y', 'http://ex/x/q/r#s'],
      ['q/r#s/t', 'http://ex/x/y', 'http://ex/x/q/r#s/t'],
      ['ftp://ex/x/q/r', 'http://ex/x/y', 'ftp://ex/x/q/r'],
      ['', 'http://ex/x/y', 'http://ex/x/y'],
      ['', 'http://ex/x/y/', 'http://ex/x/y/'],
      ['', 'http://ex/x/y/pdq', 'http://ex/x/y/pdq'],
      ['z/', 'http://ex/x/y/', 'http://ex/x/y/z/'],
      [
        '#Animal',
        'file:/swap/test/animal.rdf',
        'file:/swap/test/animal.rdf#Animal',
      ],
      ['../abc', 'file:/e/x/y/z', 'file:/e/x/abc'],
      ['/example/x/abc', 'file:/example2/x/y/z', 'file:/example/x/abc'],
      ['../r', 'file:/ex/x/y/z', 'file:/ex/x/r'],
      ['/r', 'file:/ex/x/y/z', 'file:/r'],
      ['q/r', 'file:/ex/x/y', 'file:/ex/x/q/r'],
      ['q/r#s', 'file:/ex/x/y', 'file:/ex/x/q/r#s'],
      ['q/r#', 'file:/ex/x/y', 'file:/ex/x/q/r#'],
      ['q/r#s/t', 'file:/ex/x/y', 'file:/ex/x/q/r#s/t'],
      ['ftp://ex/x/q/r', 'file:/ex/x/y', 'ftp://ex/x/q/r'],
      ['', 'file:/ex/x/y', 'file:/ex/x/y'],
      ['', 'file:/ex/x/y/', 'file:/ex/x/y/'],
      ['', 'file:/ex/x/y/pdq', 'file:/ex/x/y/pdq'],
      ['z/', 'file:/ex/x/y/', 'file:/ex/x/y/z/'],
      [
        'file://meetings.example.com/cal#m1',
        'file:/devel/WWW/2000/10/swap/test/reluri-1.n3',
        'file://meetings.example.com/cal#m1',
      ],
      [
        'file://meetings.example.com/cal#m1',
        'file:/home/connolly/w3ccvs/WWW/2000/10/swap/test/reluri-1.n3',
        'file://meetings.example.com/cal#m1',
      ],
      ['./#blort', 'file:/some/dir/foo', 'file:/some/dir/#blort'],
      ['./#', 'file:/some/dir/foo', 'file:/some/dir/#'],
      // Ryan Lee
      ['./', 'http://example/x/abc.efg', 'http://example/x/'],

      // Graham Klyne's tests
      // http://www.ninebynine.org/Software/HaskellUtils/Network/UriTest.xls
      // 01-31 are from Connelly's cases

      // 32-49
      ['./q:r', 'http://ex/x/y', 'http://ex/x/q:r'],
      ['./p=q:r', 'http://ex/x/y', 'http://ex/x/p=q:r'],
      ['?pp/rr', 'http://ex/x/y?pp/qq', 'http://ex/x/y?pp/rr'],
      ['y/z', 'http://ex/x/y?pp/qq', 'http://ex/x/y/z'],
      [
        'local/qual@domain.org#frag',
        'mailto:local',
        'mailto:local/qual@domain.org#frag',
      ],
      [
        'more/qual2@domain2.org#frag',
        'mailto:local/qual1@domain1.org',
        'mailto:local/more/qual2@domain2.org#frag',
      ],
      ['y?q', 'http://ex/x/y?q', 'http://ex/x/y?q'],
      ['/x/y?q', 'http://ex?p', 'http://ex/x/y?q'],
      ['c/d', 'foo:a/b', 'foo:a/c/d'],
      ['/c/d', 'foo:a/b', 'foo:/c/d'],
      ['', 'foo:a/b?c#d', 'foo:a/b?c'],
      ['b/c', 'foo:a', 'foo:b/c'],
      ['../b/c', 'foo:/a/y/z', 'foo:/a/b/c'],
      ['./b/c', 'foo:a', 'foo:b/c'],
      ['/./b/c', 'foo:a', 'foo:/b/c'],
      ['../../d', 'foo://a//b/c', 'foo://a/d'],
      ['.', 'foo:a', 'foo:'],
      ['..', 'foo:a', 'foo:'],

      // 50-57[cf. TimBL comments --
      //  http://lists.w3.org/Archives/Public/uri/2003Feb/0028.html,
      //  http://lists.w3.org/Archives/Public/uri/2003Jan/0008.html)
      ['abc', 'http://example/x/y%2Fz', 'http://example/x/abc'],
      ['../../x%2Fabc', 'http://example/a/x/y/z', 'http://example/a/x%2Fabc'],
      ['../x%2Fabc', 'http://example/a/x/y%2Fz', 'http://example/a/x%2Fabc'],
      ['abc', 'http://example/x%2Fy/z', 'http://example/x%2Fy/abc'],
      ['q%3Ar', 'http://ex/x/y', 'http://ex/x/q%3Ar'],
      ['/x%2Fabc', 'http://example/x/y%2Fz', 'http://example/x%2Fabc'],
      ['/x%2Fabc', 'http://example/x/y/z', 'http://example/x%2Fabc'],
      ['/x%2Fabc', 'http://example/x/y%2Fz', 'http://example/x%2Fabc'],

      // 70-77
      [
        'local2@domain2',
        'mailto:local1@domain1?query1',
        'mailto:local2@domain2',
      ],
      [
        'local2@domain2?query2',
        'mailto:local1@domain1',
        'mailto:local2@domain2?query2',
      ],
      [
        'local2@domain2?query2',
        'mailto:local1@domain1?query1',
        'mailto:local2@domain2?query2',
      ],
      ['?query2', 'mailto:local@domain?query1', 'mailto:local@domain?query2'],
      ['local@domain?query2', 'mailto:?query1', 'mailto:local@domain?query2'],
      ['?query2', 'mailto:local@domain?query1', 'mailto:local@domain?query2'],
      ['http://example/a/b?c/../d', 'foo:bar', 'http://example/a/b?c/../d'],
      ['http://example/a/b#c/../d', 'foo:bar', 'http://example/a/b#c/../d'],

      // 82-88
      // @isaacs Disagree. Not how browsers do it.
      // ['http:this', 'http://example.org/base/uri', 'http:this'],
      // @isaacs Added
      [
        'http:this',
        'http://example.org/base/uri',
        'http://example.org/base/this',
      ],
      ['http:this', 'http:base', 'http:this'],
      ['.//g', 'f:/a', 'f://g'],
      ['b/c//d/e', 'f://example.org/base/a', 'f://example.org/base/b/c//d/e'],
      [
        'm2@example.ord/c2@example.org',
        'mid:m@example.ord/c@example.org',
        'mid:m@example.ord/m2@example.ord/c2@example.org',
      ],
      [
        'mini1.xml',
        'file:///C:/DEV/Haskell/lib/HXmlToolbox-3.01/examples/',
        'file:///C:/DEV/Haskell/lib/HXmlToolbox-3.01/examples/mini1.xml',
      ],
      ['../b/c', 'foo:a/y/z', 'foo:a/b/c'],

      // changing auth
      [
        'http://diff:auth@www.example.com',
        'http://asdf:qwer@www.example.com',
        'http://diff:auth@www.example.com/',
      ],

      // changing port
      [
        'https://example.com:81/',
        'https://example.com:82/',
        'https://example.com:81/',
      ],

      // https://github.com/nodejs/node/issues/1435
      [
        'https://another.host.com/',
        'https://user:password@example.org/',
        'https://another.host.com/',
      ],
      [
        '//another.host.com/',
        'https://user:password@example.org/',
        'https://another.host.com/',
      ],
      [
        'http://another.host.com/',
        'https://user:password@example.org/',
        'http://another.host.com/',
      ],
      [
        'mailto:another.host.com',
        'mailto:user@example.org',
        'mailto:another.host.com',
      ],
      [
        'https://example.com/foo',
        'https://user:password@example.com',
        'https://user:password@example.com/foo',
      ],

      // No path at all
      ['#hash1', '#hash2', '#hash1'],
    ];
    for (let i = 0; i < relativeTests2.length; i++) {
      const relativeTest = relativeTests2[i];

      const a = url.resolve(relativeTest[1], relativeTest[0]);
      const e = url.format(relativeTest[2]);
      assert.strictEqual(
        a,
        e,
        `resolve(${relativeTest[0]}, ${relativeTest[1]})` +
          ` == ${e}\n  actual=${a}`
      );
    }

    // If format and parse are inverse operations then
    // resolveObject(parse(x), y) == parse(resolve(x, y))

    // format: [from, path, expected]
    for (let i = 0; i < relativeTests.length; i++) {
      const relativeTest = relativeTests[i];

      let actual = url.resolveObject(
        url.parse(relativeTest[0]),
        relativeTest[1]
      );
      let expected = url.parse(relativeTest[2]);

      assert.deepStrictEqual(actual, expected);

      expected = relativeTest[2];
      actual = url.format(actual);

      assert.strictEqual(
        actual,
        expected,
        `format(${actual}) == ${expected}\n` + `actual: ${actual}`
      );
    }

    // format: [to, from, result]
    // the test: ['.//g', 'f:/a', 'f://g'] is a fundamental problem
    // url.parse('f:/a') does not have a host
    // url.resolve('f:/a', './/g') does not have a host because you have moved
    // down to the g directory.  i.e. f:     //g, however when this url is parsed
    // f:// will indicate that the host is g which is not the case.
    // it is unclear to me how to keep this information from being lost
    // it may be that a pathname of ////g should collapse to /g but this seems
    // to be a lot of work for an edge case.  Right now I remove the test
    if (
      relativeTests2[181][0] === './/g' &&
      relativeTests2[181][1] === 'f:/a' &&
      relativeTests2[181][2] === 'f://g'
    ) {
      relativeTests2.splice(181, 1);
    }
    for (let i = 0; i < relativeTests2.length; i++) {
      const relativeTest = relativeTests2[i];

      let actual = url.resolveObject(
        url.parse(relativeTest[1]),
        relativeTest[0]
      );
      let expected = url.parse(relativeTest[2]);

      assert.deepStrictEqual(
        actual,
        expected,
        `expected ${inspect(expected)} but got ${inspect(actual)}`
      );

      expected = url.format(relativeTest[2]);
      actual = url.format(actual);

      assert.strictEqual(
        actual,
        expected,
        `format(${relativeTest[1]}) == ${expected}\n` + `actual: ${actual}`
      );
    }
  },
};

// Ref: https://github.com/nodejs/node/blob/e92446536ed4e268c9eef6ae6f911e384c98eecf/test/parallel/test-url-urltooptions.js
export const urlToHttpOptions = {
  async test() {
    // Test urlToHttpOptions
    const urlObj = new URL('http://user:pass@foo.bar.com:21/aaa/zzz?l=24#test');
    const opts = url.urlToHttpOptions(urlObj);
    assert.strictEqual(opts instanceof URL, false);
    assert.strictEqual(opts.protocol, 'http:');
    assert.strictEqual(opts.auth, 'user:pass');
    assert.strictEqual(opts.hostname, 'foo.bar.com');
    assert.strictEqual(opts.port, 21);
    assert.strictEqual(opts.path, '/aaa/zzz?l=24');
    assert.strictEqual(opts.pathname, '/aaa/zzz');
    assert.strictEqual(opts.search, '?l=24');
    assert.strictEqual(opts.hash, '#test');

    const { hostname } = url.urlToHttpOptions(new URL('http://[::1]:21'));
    assert.strictEqual(hostname, '::1');

    // If a WHATWG URL object is copied, it is possible that the resulting copy
    // contains the Symbols that Node uses for brand checking, but not the data
    // properties, which are getters. Verify that urlToHttpOptions() can handle
    // such a case.
    const copiedUrlObj = { ...urlObj };
    const copiedOpts = url.urlToHttpOptions(copiedUrlObj);
    assert.strictEqual(copiedOpts instanceof URL, false);
    assert.strictEqual(copiedOpts.protocol, undefined);
    assert.strictEqual(copiedOpts.auth, undefined);
    assert.strictEqual(copiedOpts.hostname, undefined);
    assert.strictEqual(copiedOpts.port, NaN);
    assert.strictEqual(copiedOpts.path, '');
    assert.strictEqual(copiedOpts.pathname, undefined);
    assert.strictEqual(copiedOpts.search, undefined);
    assert.strictEqual(copiedOpts.hash, undefined);
    assert.strictEqual(copiedOpts.href, undefined);
  },
};

// Ref: https://github.com/nodejs/node/blob/e92446536ed4e268c9eef6ae6f911e384c98eecf/test/parallel/test-url-format-whatwg.js
export const formatWhatwg = {
  async test() {
    const myURL = new URL(
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    // should format
    assert.strictEqual(
      url.format(myURL),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, {}),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    // handle invalid arguments
    for (const value of [true, 1, 'test', Infinity]) {
      assert.throws(() => url.format(myURL, value), {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      });
    }

    // any falsy value other than undefined will be treated as false
    assert.strictEqual(
      url.format(myURL, { auth: false }),
      'http://xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { auth: '' }),
      'http://xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { auth: 0 }),
      'http://xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { auth: 1 }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { auth: {} }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { fragment: false }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b'
    );

    assert.strictEqual(
      url.format(myURL, { fragment: '' }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b'
    );

    assert.strictEqual(
      url.format(myURL, { fragment: 0 }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b'
    );

    assert.strictEqual(
      url.format(myURL, { fragment: 1 }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { fragment: {} }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { search: false }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a#c'
    );

    assert.strictEqual(
      url.format(myURL, { search: '' }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a#c'
    );

    assert.strictEqual(
      url.format(myURL, { search: 0 }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a#c'
    );

    assert.strictEqual(
      url.format(myURL, { search: 1 }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { search: {} }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { unicode: true }),
      'http://user:pass@理容ナカムラ.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { unicode: 1 }),
      'http://user:pass@理容ナカムラ.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { unicode: {} }),
      'http://user:pass@理容ナカムラ.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { unicode: false }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    assert.strictEqual(
      url.format(myURL, { unicode: 0 }),
      'http://user:pass@xn--lck1c3crb1723bpq4a.com/a?a=b#c'
    );

    // should format with unicode: true
    assert.strictEqual(
      url.format(new URL('http://user:pass@xn--0zwm56d.com:8080/path'), {
        unicode: true,
      }),
      'http://user:pass@测试.com:8080/path'
    );

    // should format tel: prefix
    assert.strictEqual(
      url.format(new URL('tel:123')),
      url.format(new URL('tel:123'), { unicode: true })
    );
  },
};

// Ref: https://github.com/nodejs/node/blob/e92446536ed4e268c9eef6ae6f911e384c98eecf/test/parallel/test-url-parse-query.js
export const urlParseQuery = {
  async test() {
    function createWithNoPrototype(properties = []) {
      const noProto = { __proto__: null };
      properties.forEach((property) => {
        noProto[property.key] = property.value;
      });
      return noProto;
    }

    function check(actual, expected) {
      assert.notStrictEqual(Object.getPrototypeOf(actual), Object.prototype);
      assert.deepStrictEqual(
        Object.keys(actual).sort(),
        Object.keys(expected).sort()
      );
      Object.keys(expected).forEach(function (key) {
        assert.deepStrictEqual(actual[key], expected[key]);
      });
    }

    const parseTestsWithQueryString = {
      '/foo/bar?baz=quux#frag': {
        href: '/foo/bar?baz=quux#frag',
        hash: '#frag',
        search: '?baz=quux',
        query: createWithNoPrototype([{ key: 'baz', value: 'quux' }]),
        pathname: '/foo/bar',
        path: '/foo/bar?baz=quux',
      },
      'http://example.com': {
        href: 'http://example.com/',
        protocol: 'http:',
        slashes: true,
        host: 'example.com',
        hostname: 'example.com',
        query: createWithNoPrototype(),
        search: null,
        pathname: '/',
        path: '/',
      },
      '/example': {
        protocol: null,
        slashes: null,
        auth: undefined,
        host: null,
        port: null,
        hostname: null,
        hash: null,
        search: null,
        query: createWithNoPrototype(),
        pathname: '/example',
        path: '/example',
        href: '/example',
      },
      '/example?query=value': {
        protocol: null,
        slashes: null,
        auth: undefined,
        host: null,
        port: null,
        hostname: null,
        hash: null,
        search: '?query=value',
        query: createWithNoPrototype([{ key: 'query', value: 'value' }]),
        pathname: '/example',
        path: '/example?query=value',
        href: '/example?query=value',
      },
    };
    for (const u in parseTestsWithQueryString) {
      const actual = url.parse(u, true);
      const expected = Object.assign(
        new url.Url(),
        parseTestsWithQueryString[u]
      );
      for (const i in actual) {
        if (actual[i] === null && expected[i] === undefined) {
          expected[i] = null;
        }
      }

      const properties = Object.keys(actual).sort();
      assert.deepStrictEqual(properties, Object.keys(expected).sort());
      properties.forEach((property) => {
        if (property === 'query') {
          check(actual[property], expected[property]);
        } else {
          assert.deepStrictEqual(actual[property], expected[property]);
        }
      });
    }
  },
};

// Ref: https://github.com/nodejs/node/blob/e92446536ed4e268c9eef6ae6f911e384c98eecf/test/parallel/test-url-parse-format.js
export const urlParseFormat = {
  async test() {
    // URLs to parse, and expected data
    // { url : parsed }
    const parseTests = {
      '//some_path': {
        href: '//some_path',
        pathname: '//some_path',
        path: '//some_path',
      },

      'http:\\\\evil-phisher\\foo.html#h\\a\\s\\h': {
        protocol: 'http:',
        slashes: true,
        host: 'evil-phisher',
        hostname: 'evil-phisher',
        pathname: '/foo.html',
        path: '/foo.html',
        hash: '#h%5Ca%5Cs%5Ch',
        href: 'http://evil-phisher/foo.html#h%5Ca%5Cs%5Ch',
      },

      'http:\\\\evil-phisher\\foo.html?json="\\"foo\\""#h\\a\\s\\h': {
        protocol: 'http:',
        slashes: true,
        host: 'evil-phisher',
        hostname: 'evil-phisher',
        pathname: '/foo.html',
        search: '?json=%22%5C%22foo%5C%22%22',
        query: 'json=%22%5C%22foo%5C%22%22',
        path: '/foo.html?json=%22%5C%22foo%5C%22%22',
        hash: '#h%5Ca%5Cs%5Ch',
        href: 'http://evil-phisher/foo.html?json=%22%5C%22foo%5C%22%22#h%5Ca%5Cs%5Ch',
      },

      'http:\\\\evil-phisher\\foo.html#h\\a\\s\\h?blarg': {
        protocol: 'http:',
        slashes: true,
        host: 'evil-phisher',
        hostname: 'evil-phisher',
        pathname: '/foo.html',
        path: '/foo.html',
        hash: '#h%5Ca%5Cs%5Ch?blarg',
        href: 'http://evil-phisher/foo.html#h%5Ca%5Cs%5Ch?blarg',
      },

      'http:\\\\evil-phisher\\foo.html': {
        protocol: 'http:',
        slashes: true,
        host: 'evil-phisher',
        hostname: 'evil-phisher',
        pathname: '/foo.html',
        path: '/foo.html',
        href: 'http://evil-phisher/foo.html',
      },

      'HTTP://www.example.com/': {
        href: 'http://www.example.com/',
        protocol: 'http:',
        slashes: true,
        host: 'www.example.com',
        hostname: 'www.example.com',
        pathname: '/',
        path: '/',
      },

      'HTTP://www.example.com': {
        href: 'http://www.example.com/',
        protocol: 'http:',
        slashes: true,
        host: 'www.example.com',
        hostname: 'www.example.com',
        pathname: '/',
        path: '/',
      },

      'http://www.ExAmPlE.com/': {
        href: 'http://www.example.com/',
        protocol: 'http:',
        slashes: true,
        host: 'www.example.com',
        hostname: 'www.example.com',
        pathname: '/',
        path: '/',
      },

      'http://user:pw@www.ExAmPlE.com/': {
        href: 'http://user:pw@www.example.com/',
        protocol: 'http:',
        slashes: true,
        auth: 'user:pw',
        host: 'www.example.com',
        hostname: 'www.example.com',
        pathname: '/',
        path: '/',
      },

      'http://USER:PW@www.ExAmPlE.com/': {
        href: 'http://USER:PW@www.example.com/',
        protocol: 'http:',
        slashes: true,
        auth: 'USER:PW',
        host: 'www.example.com',
        hostname: 'www.example.com',
        pathname: '/',
        path: '/',
      },

      'http://user@www.example.com/': {
        href: 'http://user@www.example.com/',
        protocol: 'http:',
        slashes: true,
        auth: 'user',
        host: 'www.example.com',
        hostname: 'www.example.com',
        pathname: '/',
        path: '/',
      },

      'http://user%3Apw@www.example.com/': {
        href: 'http://user:pw@www.example.com/',
        protocol: 'http:',
        slashes: true,
        auth: 'user:pw',
        host: 'www.example.com',
        hostname: 'www.example.com',
        pathname: '/',
        path: '/',
      },

      "http://x.com/path?that's#all, folks": {
        href: 'http://x.com/path?that%27s#all,%20folks',
        protocol: 'http:',
        slashes: true,
        host: 'x.com',
        hostname: 'x.com',
        search: '?that%27s',
        query: 'that%27s',
        pathname: '/path',
        hash: '#all,%20folks',
        path: '/path?that%27s',
      },

      'HTTP://X.COM/Y': {
        href: 'http://x.com/Y',
        protocol: 'http:',
        slashes: true,
        host: 'x.com',
        hostname: 'x.com',
        pathname: '/Y',
        path: '/Y',
      },

      // Whitespace in the front
      ' http://www.example.com/': {
        href: 'http://www.example.com/',
        protocol: 'http:',
        slashes: true,
        host: 'www.example.com',
        hostname: 'www.example.com',
        pathname: '/',
        path: '/',
      },

      // + not an invalid host character
      // per https://url.spec.whatwg.org/#host-parsing
      'http://x.y.com+a/b/c': {
        href: 'http://x.y.com+a/b/c',
        protocol: 'http:',
        slashes: true,
        host: 'x.y.com+a',
        hostname: 'x.y.com+a',
        pathname: '/b/c',
        path: '/b/c',
      },

      // An unexpected invalid char in the hostname.
      'HtTp://x.y.cOm;a/b/c?d=e#f g<h>i': {
        href: 'http://x.y.com/;a/b/c?d=e#f%20g%3Ch%3Ei',
        protocol: 'http:',
        slashes: true,
        host: 'x.y.com',
        hostname: 'x.y.com',
        pathname: ';a/b/c',
        search: '?d=e',
        query: 'd=e',
        hash: '#f%20g%3Ch%3Ei',
        path: ';a/b/c?d=e',
      },

      // Make sure that we don't accidentally lcast the path parts.
      'HtTp://x.y.cOm;A/b/c?d=e#f g<h>i': {
        href: 'http://x.y.com/;A/b/c?d=e#f%20g%3Ch%3Ei',
        protocol: 'http:',
        slashes: true,
        host: 'x.y.com',
        hostname: 'x.y.com',
        pathname: ';A/b/c',
        search: '?d=e',
        query: 'd=e',
        hash: '#f%20g%3Ch%3Ei',
        path: ';A/b/c?d=e',
      },

      'http://x...y...#p': {
        href: 'http://x...y.../#p',
        protocol: 'http:',
        slashes: true,
        host: 'x...y...',
        hostname: 'x...y...',
        hash: '#p',
        pathname: '/',
        path: '/',
      },

      'http://x/p/"quoted"': {
        href: 'http://x/p/%22quoted%22',
        protocol: 'http:',
        slashes: true,
        host: 'x',
        hostname: 'x',
        pathname: '/p/%22quoted%22',
        path: '/p/%22quoted%22',
      },

      '<http://goo.corn/bread> Is a URL!': {
        href: '%3Chttp://goo.corn/bread%3E%20Is%20a%20URL!',
        pathname: '%3Chttp://goo.corn/bread%3E%20Is%20a%20URL!',
        path: '%3Chttp://goo.corn/bread%3E%20Is%20a%20URL!',
      },

      'http://www.narwhaljs.org/blog/categories?id=news': {
        href: 'http://www.narwhaljs.org/blog/categories?id=news',
        protocol: 'http:',
        slashes: true,
        host: 'www.narwhaljs.org',
        hostname: 'www.narwhaljs.org',
        search: '?id=news',
        query: 'id=news',
        pathname: '/blog/categories',
        path: '/blog/categories?id=news',
      },

      'http://mt0.google.com/vt/lyrs=m@114&hl=en&src=api&x=2&y=2&z=3&s=': {
        href: 'http://mt0.google.com/vt/lyrs=m@114&hl=en&src=api&x=2&y=2&z=3&s=',
        protocol: 'http:',
        slashes: true,
        host: 'mt0.google.com',
        hostname: 'mt0.google.com',
        pathname: '/vt/lyrs=m@114&hl=en&src=api&x=2&y=2&z=3&s=',
        path: '/vt/lyrs=m@114&hl=en&src=api&x=2&y=2&z=3&s=',
      },

      'http://mt0.google.com/vt/lyrs=m@114???&hl=en&src=api&x=2&y=2&z=3&s=': {
        href:
          'http://mt0.google.com/vt/lyrs=m@114???&hl=en&src=api' +
          '&x=2&y=2&z=3&s=',
        protocol: 'http:',
        slashes: true,
        host: 'mt0.google.com',
        hostname: 'mt0.google.com',
        search: '???&hl=en&src=api&x=2&y=2&z=3&s=',
        query: '??&hl=en&src=api&x=2&y=2&z=3&s=',
        pathname: '/vt/lyrs=m@114',
        path: '/vt/lyrs=m@114???&hl=en&src=api&x=2&y=2&z=3&s=',
      },

      'http://user:pass@mt0.google.com/vt/lyrs=m@114???&hl=en&src=api&x=2&y=2&z=3&s=':
        {
          href: 'http://user:pass@mt0.google.com/vt/lyrs=m@114???&hl=en&src=api&x=2&y=2&z=3&s=',
          protocol: 'http:',
          slashes: true,
          host: 'mt0.google.com',
          auth: 'user:pass',
          hostname: 'mt0.google.com',
          search: '???&hl=en&src=api&x=2&y=2&z=3&s=',
          query: '??&hl=en&src=api&x=2&y=2&z=3&s=',
          pathname: '/vt/lyrs=m@114',
          path: '/vt/lyrs=m@114???&hl=en&src=api&x=2&y=2&z=3&s=',
        },

      'file:///etc/passwd': {
        href: 'file:///etc/passwd',
        slashes: true,
        protocol: 'file:',
        pathname: '/etc/passwd',
        hostname: '',
        host: '',
        path: '/etc/passwd',
      },

      'file://localhost/etc/passwd': {
        href: 'file://localhost/etc/passwd',
        protocol: 'file:',
        slashes: true,
        pathname: '/etc/passwd',
        hostname: 'localhost',
        host: 'localhost',
        path: '/etc/passwd',
      },

      'file://foo/etc/passwd': {
        href: 'file://foo/etc/passwd',
        protocol: 'file:',
        slashes: true,
        pathname: '/etc/passwd',
        hostname: 'foo',
        host: 'foo',
        path: '/etc/passwd',
      },

      'file:///etc/node/': {
        href: 'file:///etc/node/',
        slashes: true,
        protocol: 'file:',
        pathname: '/etc/node/',
        hostname: '',
        host: '',
        path: '/etc/node/',
      },

      'file://localhost/etc/node/': {
        href: 'file://localhost/etc/node/',
        protocol: 'file:',
        slashes: true,
        pathname: '/etc/node/',
        hostname: 'localhost',
        host: 'localhost',
        path: '/etc/node/',
      },

      'file://foo/etc/node/': {
        href: 'file://foo/etc/node/',
        protocol: 'file:',
        slashes: true,
        pathname: '/etc/node/',
        hostname: 'foo',
        host: 'foo',
        path: '/etc/node/',
      },

      'http:/baz/../foo/bar': {
        href: 'http:/baz/../foo/bar',
        protocol: 'http:',
        pathname: '/baz/../foo/bar',
        path: '/baz/../foo/bar',
      },

      'http://user:pass@example.com:8000/foo/bar?baz=quux#frag': {
        href: 'http://user:pass@example.com:8000/foo/bar?baz=quux#frag',
        protocol: 'http:',
        slashes: true,
        host: 'example.com:8000',
        auth: 'user:pass',
        port: '8000',
        hostname: 'example.com',
        hash: '#frag',
        search: '?baz=quux',
        query: 'baz=quux',
        pathname: '/foo/bar',
        path: '/foo/bar?baz=quux',
      },

      '//user:pass@example.com:8000/foo/bar?baz=quux#frag': {
        href: '//user:pass@example.com:8000/foo/bar?baz=quux#frag',
        slashes: true,
        host: 'example.com:8000',
        auth: 'user:pass',
        port: '8000',
        hostname: 'example.com',
        hash: '#frag',
        search: '?baz=quux',
        query: 'baz=quux',
        pathname: '/foo/bar',
        path: '/foo/bar?baz=quux',
      },

      '/foo/bar?baz=quux#frag': {
        href: '/foo/bar?baz=quux#frag',
        hash: '#frag',
        search: '?baz=quux',
        query: 'baz=quux',
        pathname: '/foo/bar',
        path: '/foo/bar?baz=quux',
      },

      'http:/foo/bar?baz=quux#frag': {
        href: 'http:/foo/bar?baz=quux#frag',
        protocol: 'http:',
        hash: '#frag',
        search: '?baz=quux',
        query: 'baz=quux',
        pathname: '/foo/bar',
        path: '/foo/bar?baz=quux',
      },

      'mailto:foo@bar.com?subject=hello': {
        href: 'mailto:foo@bar.com?subject=hello',
        protocol: 'mailto:',
        host: 'bar.com',
        auth: 'foo',
        hostname: 'bar.com',
        search: '?subject=hello',
        query: 'subject=hello',
        path: '?subject=hello',
      },

      "javascript:alert('hello');": {
        href: "javascript:alert('hello');",
        protocol: 'javascript:',
        pathname: "alert('hello');",
        path: "alert('hello');",
      },

      'xmpp:isaacschlueter@jabber.org': {
        href: 'xmpp:isaacschlueter@jabber.org',
        protocol: 'xmpp:',
        host: 'jabber.org',
        auth: 'isaacschlueter',
        hostname: 'jabber.org',
      },

      'http://atpass:foo%40bar@127.0.0.1:8080/path?search=foo#bar': {
        href: 'http://atpass:foo%40bar@127.0.0.1:8080/path?search=foo#bar',
        protocol: 'http:',
        slashes: true,
        host: '127.0.0.1:8080',
        auth: 'atpass:foo@bar',
        hostname: '127.0.0.1',
        port: '8080',
        pathname: '/path',
        search: '?search=foo',
        query: 'search=foo',
        hash: '#bar',
        path: '/path?search=foo',
      },

      'svn+ssh://foo/bar': {
        href: 'svn+ssh://foo/bar',
        host: 'foo',
        hostname: 'foo',
        protocol: 'svn+ssh:',
        pathname: '/bar',
        path: '/bar',
        slashes: true,
      },

      'dash-test://foo/bar': {
        href: 'dash-test://foo/bar',
        host: 'foo',
        hostname: 'foo',
        protocol: 'dash-test:',
        pathname: '/bar',
        path: '/bar',
        slashes: true,
      },

      'dash-test:foo/bar': {
        href: 'dash-test:foo/bar',
        host: 'foo',
        hostname: 'foo',
        protocol: 'dash-test:',
        pathname: '/bar',
        path: '/bar',
      },

      'dot.test://foo/bar': {
        href: 'dot.test://foo/bar',
        host: 'foo',
        hostname: 'foo',
        protocol: 'dot.test:',
        pathname: '/bar',
        path: '/bar',
        slashes: true,
      },

      'dot.test:foo/bar': {
        href: 'dot.test:foo/bar',
        host: 'foo',
        hostname: 'foo',
        protocol: 'dot.test:',
        pathname: '/bar',
        path: '/bar',
      },

      // IDNA tests
      'http://www.日本語.com/': {
        href: 'http://www.xn--wgv71a119e.com/',
        protocol: 'http:',
        slashes: true,
        host: 'www.xn--wgv71a119e.com',
        hostname: 'www.xn--wgv71a119e.com',
        pathname: '/',
        path: '/',
      },

      'http://example.Bücher.com/': {
        href: 'http://example.xn--bcher-kva.com/',
        protocol: 'http:',
        slashes: true,
        host: 'example.xn--bcher-kva.com',
        hostname: 'example.xn--bcher-kva.com',
        pathname: '/',
        path: '/',
      },

      'http://www.Äffchen.com/': {
        href: 'http://www.xn--ffchen-9ta.com/',
        protocol: 'http:',
        slashes: true,
        host: 'www.xn--ffchen-9ta.com',
        hostname: 'www.xn--ffchen-9ta.com',
        pathname: '/',
        path: '/',
      },

      'http://www.Äffchen.cOm;A/b/c?d=e#f g<h>i': {
        href: 'http://www.xn--ffchen-9ta.com/;A/b/c?d=e#f%20g%3Ch%3Ei',
        protocol: 'http:',
        slashes: true,
        host: 'www.xn--ffchen-9ta.com',
        hostname: 'www.xn--ffchen-9ta.com',
        pathname: ';A/b/c',
        search: '?d=e',
        query: 'd=e',
        hash: '#f%20g%3Ch%3Ei',
        path: ';A/b/c?d=e',
      },

      'http://SÉLIER.COM/': {
        href: 'http://xn--slier-bsa.com/',
        protocol: 'http:',
        slashes: true,
        host: 'xn--slier-bsa.com',
        hostname: 'xn--slier-bsa.com',
        pathname: '/',
        path: '/',
      },

      'http://ليهمابتكلموشعربي؟.ي؟/': {
        href: 'http://xn--egbpdaj6bu4bxfgehfvwxn.xn--egb9f/',
        protocol: 'http:',
        slashes: true,
        host: 'xn--egbpdaj6bu4bxfgehfvwxn.xn--egb9f',
        hostname: 'xn--egbpdaj6bu4bxfgehfvwxn.xn--egb9f',
        pathname: '/',
        path: '/',
      },

      'http://➡.ws/➡': {
        href: 'http://xn--hgi.ws/➡',
        protocol: 'http:',
        slashes: true,
        host: 'xn--hgi.ws',
        hostname: 'xn--hgi.ws',
        pathname: '/➡',
        path: '/➡',
      },

      'http://bucket_name.s3.amazonaws.com/image.jpg': {
        protocol: 'http:',
        slashes: true,
        host: 'bucket_name.s3.amazonaws.com',
        hostname: 'bucket_name.s3.amazonaws.com',
        pathname: '/image.jpg',
        href: 'http://bucket_name.s3.amazonaws.com/image.jpg',
        path: '/image.jpg',
      },

      'git+http://github.com/joyent/node.git': {
        protocol: 'git+http:',
        slashes: true,
        host: 'github.com',
        hostname: 'github.com',
        pathname: '/joyent/node.git',
        path: '/joyent/node.git',
        href: 'git+http://github.com/joyent/node.git',
      },

      // If local1@domain1 is uses as a relative URL it may
      // be parse into auth@hostname, but here there is no
      // way to make it work in url.parse, I add the test to be explicit
      'local1@domain1': {
        pathname: 'local1@domain1',
        path: 'local1@domain1',
        href: 'local1@domain1',
      },

      // While this may seem counter-intuitive, a browser will parse
      // <a href='www.google.com'> as a path.
      'www.example.com': {
        href: 'www.example.com',
        pathname: 'www.example.com',
        path: 'www.example.com',
      },

      // ipv6 support
      '[fe80::1]': {
        href: '[fe80::1]',
        pathname: '[fe80::1]',
        path: '[fe80::1]',
      },

      'coap://[FEDC:BA98:7654:3210:FEDC:BA98:7654:3210]': {
        protocol: 'coap:',
        slashes: true,
        host: '[fedc:ba98:7654:3210:fedc:ba98:7654:3210]',
        hostname: 'fedc:ba98:7654:3210:fedc:ba98:7654:3210',
        href: 'coap://[fedc:ba98:7654:3210:fedc:ba98:7654:3210]/',
        pathname: '/',
        path: '/',
      },

      'coap://[1080:0:0:0:8:800:200C:417A]:61616/': {
        protocol: 'coap:',
        slashes: true,
        host: '[1080:0:0:0:8:800:200c:417a]:61616',
        port: '61616',
        hostname: '1080:0:0:0:8:800:200c:417a',
        href: 'coap://[1080:0:0:0:8:800:200c:417a]:61616/',
        pathname: '/',
        path: '/',
      },

      'http://user:password@[3ffe:2a00:100:7031::1]:8080': {
        protocol: 'http:',
        slashes: true,
        auth: 'user:password',
        host: '[3ffe:2a00:100:7031::1]:8080',
        port: '8080',
        hostname: '3ffe:2a00:100:7031::1',
        href: 'http://user:password@[3ffe:2a00:100:7031::1]:8080/',
        pathname: '/',
        path: '/',
      },

      'coap://u:p@[::192.9.5.5]:61616/.well-known/r?n=Temperature': {
        protocol: 'coap:',
        slashes: true,
        auth: 'u:p',
        host: '[::192.9.5.5]:61616',
        port: '61616',
        hostname: '::192.9.5.5',
        href: 'coap://u:p@[::192.9.5.5]:61616/.well-known/r?n=Temperature',
        search: '?n=Temperature',
        query: 'n=Temperature',
        pathname: '/.well-known/r',
        path: '/.well-known/r?n=Temperature',
      },

      // empty port
      'http://example.com:': {
        protocol: 'http:',
        slashes: true,
        host: 'example.com',
        hostname: 'example.com',
        href: 'http://example.com/',
        pathname: '/',
        path: '/',
      },

      'http://example.com:/a/b.html': {
        protocol: 'http:',
        slashes: true,
        host: 'example.com',
        hostname: 'example.com',
        href: 'http://example.com/a/b.html',
        pathname: '/a/b.html',
        path: '/a/b.html',
      },

      'http://example.com:?a=b': {
        protocol: 'http:',
        slashes: true,
        host: 'example.com',
        hostname: 'example.com',
        href: 'http://example.com/?a=b',
        search: '?a=b',
        query: 'a=b',
        pathname: '/',
        path: '/?a=b',
      },

      'http://example.com:#abc': {
        protocol: 'http:',
        slashes: true,
        host: 'example.com',
        hostname: 'example.com',
        href: 'http://example.com/#abc',
        hash: '#abc',
        pathname: '/',
        path: '/',
      },

      'http://[fe80::1]:/a/b?a=b#abc': {
        protocol: 'http:',
        slashes: true,
        host: '[fe80::1]',
        hostname: 'fe80::1',
        href: 'http://[fe80::1]/a/b?a=b#abc',
        search: '?a=b',
        query: 'a=b',
        hash: '#abc',
        pathname: '/a/b',
        path: '/a/b?a=b',
      },

      'http://-lovemonsterz.tumblr.com/rss': {
        protocol: 'http:',
        slashes: true,
        host: '-lovemonsterz.tumblr.com',
        hostname: '-lovemonsterz.tumblr.com',
        href: 'http://-lovemonsterz.tumblr.com/rss',
        pathname: '/rss',
        path: '/rss',
      },

      'http://-lovemonsterz.tumblr.com:80/rss': {
        protocol: 'http:',
        slashes: true,
        port: '80',
        host: '-lovemonsterz.tumblr.com:80',
        hostname: '-lovemonsterz.tumblr.com',
        href: 'http://-lovemonsterz.tumblr.com:80/rss',
        pathname: '/rss',
        path: '/rss',
      },

      'http://user:pass@-lovemonsterz.tumblr.com/rss': {
        protocol: 'http:',
        slashes: true,
        auth: 'user:pass',
        host: '-lovemonsterz.tumblr.com',
        hostname: '-lovemonsterz.tumblr.com',
        href: 'http://user:pass@-lovemonsterz.tumblr.com/rss',
        pathname: '/rss',
        path: '/rss',
      },

      'http://user:pass@-lovemonsterz.tumblr.com:80/rss': {
        protocol: 'http:',
        slashes: true,
        auth: 'user:pass',
        port: '80',
        host: '-lovemonsterz.tumblr.com:80',
        hostname: '-lovemonsterz.tumblr.com',
        href: 'http://user:pass@-lovemonsterz.tumblr.com:80/rss',
        pathname: '/rss',
        path: '/rss',
      },

      'http://_jabber._tcp.google.com/test': {
        protocol: 'http:',
        slashes: true,
        host: '_jabber._tcp.google.com',
        hostname: '_jabber._tcp.google.com',
        href: 'http://_jabber._tcp.google.com/test',
        pathname: '/test',
        path: '/test',
      },

      'http://user:pass@_jabber._tcp.google.com/test': {
        protocol: 'http:',
        slashes: true,
        auth: 'user:pass',
        host: '_jabber._tcp.google.com',
        hostname: '_jabber._tcp.google.com',
        href: 'http://user:pass@_jabber._tcp.google.com/test',
        pathname: '/test',
        path: '/test',
      },

      'http://_jabber._tcp.google.com:80/test': {
        protocol: 'http:',
        slashes: true,
        port: '80',
        host: '_jabber._tcp.google.com:80',
        hostname: '_jabber._tcp.google.com',
        href: 'http://_jabber._tcp.google.com:80/test',
        pathname: '/test',
        path: '/test',
      },

      'http://user:pass@_jabber._tcp.google.com:80/test': {
        protocol: 'http:',
        slashes: true,
        auth: 'user:pass',
        port: '80',
        host: '_jabber._tcp.google.com:80',
        hostname: '_jabber._tcp.google.com',
        href: 'http://user:pass@_jabber._tcp.google.com:80/test',
        pathname: '/test',
        path: '/test',
      },

      'http://x:1/\' <>"`/{}|\\^~`/': {
        protocol: 'http:',
        slashes: true,
        host: 'x:1',
        port: '1',
        hostname: 'x',
        pathname: '/%27%20%3C%3E%22%60/%7B%7D%7C/%5E~%60/',
        path: '/%27%20%3C%3E%22%60/%7B%7D%7C/%5E~%60/',
        href: 'http://x:1/%27%20%3C%3E%22%60/%7B%7D%7C/%5E~%60/',
      },

      'http://a@b@c/': {
        protocol: 'http:',
        slashes: true,
        auth: 'a@b',
        host: 'c',
        hostname: 'c',
        href: 'http://a%40b@c/',
        path: '/',
        pathname: '/',
      },

      'http://a@b?@c': {
        protocol: 'http:',
        slashes: true,
        auth: 'a',
        host: 'b',
        hostname: 'b',
        href: 'http://a@b/?@c',
        path: '/?@c',
        pathname: '/',
        search: '?@c',
        query: '@c',
      },

      'http://a.b/\tbc\ndr\ref g"hq\'j<kl>?mn\\op^q=r`99{st|uv}wz': {
        protocol: 'http:',
        slashes: true,
        host: 'a.b',
        port: null,
        hostname: 'a.b',
        hash: null,
        pathname: '/%09bc%0Adr%0Def%20g%22hq%27j%3Ckl%3E',
        path: '/%09bc%0Adr%0Def%20g%22hq%27j%3Ckl%3E?mn%5Cop%5Eq=r%6099%7Bst%7Cuv%7Dwz',
        search: '?mn%5Cop%5Eq=r%6099%7Bst%7Cuv%7Dwz',
        query: 'mn%5Cop%5Eq=r%6099%7Bst%7Cuv%7Dwz',
        href: 'http://a.b/%09bc%0Adr%0Def%20g%22hq%27j%3Ckl%3E?mn%5Cop%5Eq=r%6099%7Bst%7Cuv%7Dwz',
      },

      'http://a\r" \t\n<\'b:b@c\r\nd/e?f': {
        protocol: 'http:',
        slashes: true,
        auth: 'a" <\'b:b',
        host: 'cd',
        port: null,
        hostname: 'cd',
        hash: null,
        search: '?f',
        query: 'f',
        pathname: '/e',
        path: '/e?f',
        href: "http://a%22%20%3C'b:b@cd/e?f",
      },

      // Git urls used by npm
      'git+ssh://git@github.com:npm/npm': {
        protocol: 'git+ssh:',
        slashes: true,
        auth: 'git',
        host: 'github.com',
        port: null,
        hostname: 'github.com',
        hash: null,
        search: null,
        query: null,
        pathname: '/:npm/npm',
        path: '/:npm/npm',
        href: 'git+ssh://git@github.com/:npm/npm',
      },

      'https://*': {
        protocol: 'https:',
        slashes: true,
        auth: null,
        host: '*',
        port: null,
        hostname: '*',
        hash: null,
        search: null,
        query: null,
        pathname: '/',
        path: '/',
        href: 'https://*/',
      },

      // The following two URLs are the same, but they differ for a capital A.
      // Verify that the protocol is checked in a case-insensitive manner.
      'javascript:alert(1);a=\x27@white-listed.com\x27': {
        protocol: 'javascript:',
        slashes: null,
        auth: null,
        host: null,
        port: null,
        hostname: null,
        hash: null,
        search: null,
        query: null,
        pathname: "alert(1);a='@white-listed.com'",
        path: "alert(1);a='@white-listed.com'",
        href: "javascript:alert(1);a='@white-listed.com'",
      },

      'javAscript:alert(1);a=\x27@white-listed.com\x27': {
        protocol: 'javascript:',
        slashes: null,
        auth: null,
        host: null,
        port: null,
        hostname: null,
        hash: null,
        search: null,
        query: null,
        pathname: "alert(1);a='@white-listed.com'",
        path: "alert(1);a='@white-listed.com'",
        href: "javascript:alert(1);a='@white-listed.com'",
      },

      'ws://www.example.com': {
        protocol: 'ws:',
        slashes: true,
        hostname: 'www.example.com',
        host: 'www.example.com',
        pathname: '/',
        path: '/',
        href: 'ws://www.example.com/',
      },

      'wss://www.example.com': {
        protocol: 'wss:',
        slashes: true,
        hostname: 'www.example.com',
        host: 'www.example.com',
        pathname: '/',
        path: '/',
        href: 'wss://www.example.com/',
      },

      '//fhqwhgads@example.com/everybody-to-the-limit': {
        protocol: null,
        slashes: true,
        auth: 'fhqwhgads',
        host: 'example.com',
        port: null,
        hostname: 'example.com',
        hash: null,
        search: null,
        query: null,
        pathname: '/everybody-to-the-limit',
        path: '/everybody-to-the-limit',
        href: '//fhqwhgads@example.com/everybody-to-the-limit',
      },

      '//fhqwhgads@example.com/everybody#to-the-limit': {
        protocol: null,
        slashes: true,
        auth: 'fhqwhgads',
        host: 'example.com',
        port: null,
        hostname: 'example.com',
        hash: '#to-the-limit',
        search: null,
        query: null,
        pathname: '/everybody',
        path: '/everybody',
        href: '//fhqwhgads@example.com/everybody#to-the-limit',
      },

      '\bhttp://example.com/\b': {
        protocol: 'http:',
        slashes: true,
        auth: null,
        host: 'example.com',
        port: null,
        hostname: 'example.com',
        hash: null,
        search: null,
        query: null,
        pathname: '/',
        path: '/',
        href: 'http://example.com/',
      },

      'https://evil.com$.example.com': {
        protocol: 'https:',
        slashes: true,
        auth: null,
        host: 'evil.com$.example.com',
        port: null,
        hostname: 'evil.com$.example.com',
        hash: null,
        search: null,
        query: null,
        pathname: '/',
        path: '/',
        href: 'https://evil.com$.example.com/',
      },

      // Validate the output of hostname with commas.
      'x://0.0,1.1/': {
        protocol: 'x:',
        slashes: true,
        auth: null,
        host: '0.0,1.1',
        port: null,
        hostname: '0.0,1.1',
        hash: null,
        search: null,
        query: null,
        pathname: '/',
        path: '/',
        href: 'x://0.0,1.1/',
      },
    };

    // should parse and format
    {
      for (const u in parseTests) {
        let actual = url.parse(u);
        const spaced = url.parse(`     \t  ${u}\n\t`);
        let expected = Object.assign(new url.Url(), parseTests[u]);

        Object.keys(actual).forEach(function (i) {
          if (expected[i] === undefined && actual[i] === null) {
            expected[i] = null;
          }
        });

        assert.deepStrictEqual(
          actual,
          expected,
          `parsing ${u} and expected ${inspect(expected)} but got ${inspect(actual)}`
        );
        assert.deepStrictEqual(
          spaced,
          expected,
          `expected ${inspect(expected)}, got ${inspect(spaced)}`
        );

        expected = parseTests[u].href;
        actual = url.format(parseTests[u]);

        assert.strictEqual(
          actual,
          expected,
          `format(${u}) == ${u}\nactual:${actual}`
        );
      }
    }

    // parse result should equal new url.Url()
    {
      const parsed = url
        .parse('http://nodejs.org/')
        .resolveObject('jAvascript:alert(1);a=\x27@white-listed.com\x27');

      const expected = Object.assign(new url.Url(), {
        protocol: 'javascript:',
        slashes: null,
        auth: null,
        host: null,
        port: null,
        hostname: null,
        hash: null,
        search: null,
        query: null,
        pathname: "alert(1);a='@white-listed.com'",
        path: "alert(1);a='@white-listed.com'",
        href: "javascript:alert(1);a='@white-listed.com'",
      });

      assert.deepStrictEqual(parsed, expected);
    }
  },
};

// Ref: https://github.com/nodejs/node/blob/e92446536ed4e268c9eef6ae6f911e384c98eecf/test/parallel/test-url-parse-invalid-input.js
export const urlParseInvalidInput = {
  async test() {
    // https://github.com/joyent/node/issues/568
    [
      [undefined, 'undefined'],
      [null, 'object'],
      [true, 'boolean'],
      [false, 'boolean'],
      [0.0, 'number'],
      [0, 'number'],
      [[], 'object'],
      [{}, 'object'],
      [() => {}, 'function'],
      [Symbol('foo'), 'symbol'],
    ].forEach(([val, type]) => {
      assert.throws(
        () => {
          url.parse(val);
        },
        {
          code: 'ERR_INVALID_ARG_TYPE',
          name: 'TypeError',
        }
      );
    });

    assert.throws(
      () => {
        url.parse('http://%E0%A4%A@fail');
      },
      (e) => {
        // The error should be a URIError.
        if (!(e instanceof URIError)) return false;

        // The error should be from the JS engine and not from Node.js.
        // JS engine errors do not have the `code` property.
        return e.code === undefined;
      }
    );

    assert.throws(
      () => {
        url.parse('http://[127.0.0.1\x00c8763]:8000/');
      },
      { code: 'ERR_INVALID_URL', input: 'http://[127.0.0.1\x00c8763]:8000/' }
    );

    {
      // An array of Unicode code points whose Unicode NFKD contains a "bad
      // character".
      const badIDNA = (() => {
        const BAD_CHARS = '#%/:?@[\\]^|';
        const out = [];
        for (let i = 0x80; i < 0x110000; i++) {
          const cp = String.fromCodePoint(i);
          for (const badChar of BAD_CHARS) {
            if (cp.normalize('NFKD').includes(badChar)) {
              out.push(cp);
            }
          }
        }
        return out;
      })();

      // The generation logic above should at a minimum produce these two
      // characters.
      assert(badIDNA.includes('℀'));
      assert(badIDNA.includes('＠'));

      for (const badCodePoint of badIDNA) {
        const badURL = `http://fail${badCodePoint}fail.com/`;
        assert.throws(
          () => {
            url.parse(badURL);
          },
          (e) => e.code === 'ERR_INVALID_URL',
          `parsing ${badURL}`
        );
      }

      assert.throws(
        () => {
          url.parse('http://\u00AD/bad.com/');
        },
        (e) => e.code === 'ERR_INVALID_URL',
        'parsing http://\u00AD/bad.com/'
      );
    }
  },
};
