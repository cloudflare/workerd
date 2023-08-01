// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export const test_global_constructor = {
  async test() {
    const urlPattern = new URLPattern();

    // The standard attributes exist as they should and are readonly
    assert.equal(urlPattern.protocol, "*");
    assert.equal(urlPattern.username, "*");
    assert.equal(urlPattern.password, "*");
    assert.equal(urlPattern.hostname, "*");
    assert.equal(urlPattern.port, "*");
    assert.equal(urlPattern.pathname, "*");
    assert.equal(urlPattern.search, "*");
    assert.equal(urlPattern.hash, "*");

    assert.equal(typeof urlPattern.exec, 'function');
    assert.equal(typeof urlPattern.test, 'function');

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
      assert.equal(myPattern.protocol, "*");
      assert.equal(myPattern.username, "*");
      assert.equal(myPattern.password, "*");
      assert.equal(myPattern.hostname, "*");
      assert.equal(myPattern.port, "*");
      assert.equal(myPattern.pathname, "*");
      assert.equal(myPattern.search, "*");
      assert.equal(myPattern.hash, "*");
      assert.equal(myPattern.exec(), true);
      assert.equal(myPattern.test(), true);
      assert.equal(count, 8);
    }
  }
}

export const test_readonly_property = {
  async test() {
    const urlPattern = new URLPattern();

    // Setting the values should throw effect.
    assert.throws(() => { urlPattern.protocol = 1 }, TypeError);
    assert.throws(() => { urlPattern.username = 1 }, TypeError);
    assert.throws(() => { urlPattern.password = 1 }, TypeError);
    assert.throws(() => { urlPattern.hostname = 1 }, TypeError);
    assert.throws(() => { urlPattern.port = 1 }, TypeError);
    assert.throws(() => { urlPattern.pathname = 1 }, TypeError);
    assert.throws(() => { urlPattern.search = 1 }, TypeError);
    assert.throws(() => { urlPattern.hash = 1 }, TypeError);
  }
}

export const test_mdn_1 = {
  async test() {
    const pattern = new URLPattern({ pathname: '/books' });
    assert.ok(pattern.test('https://example.com/books'));

    const res = pattern.exec('https://example.com/books');
    assert.equal(res.inputs[0], 'https://example.com/books');
    assert.equal(res.protocol.input, 'https');
    assert.equal(res.protocol.groups["0"], "https");
    assert.equal(res.username.input, "");
    assert.equal(res.username.groups["0"], "");
    assert.equal(res.password.input, "");
    assert.equal(res.password.groups["0"], "");
    assert.equal(res.hostname.input, "example.com");
    assert.equal(res.hostname.groups["0"], "example.com");
    assert.equal(res.pathname.input, "/books");
    assert.deepEqual(Object.keys(res.pathname.groups), []);
    assert.equal(res.search.input, "");
    assert.equal(res.search.groups["0"], "");
    assert.equal(res.hash.input, "");
    assert.equal(res.hash.groups["0"], "");
  }
}

export const test_mdn_2 = {
  async test() {
    const pattern = new URLPattern({ pathname: '/books/:id' });
    assert.ok(pattern.test('https://example.com/books/123'));
    assert.equal(pattern.exec('https://example.com/books/123').pathname.groups.id, "123");
  }
}

export const test_mdn_3 = {
  async test() {
    const pattern = new URLPattern('/books/:id(\\d+)', 'https://example.com');
    assert.ok(pattern.test('https://example.com/books/123'));
    assert.ok(!pattern.test('https://example.com/books/abc'));
    assert.ok(!pattern.test('https://example.com/books/'));
  }
}

export const test_mdn_4 = {
  async test() {
    const pattern = new URLPattern({ pathname: '/:type(foo|bar)' });
    const result = pattern.exec({ pathname: '/foo' });
    assert.equal(result.pathname.groups.type, 'foo');
  }
}

export const test_mdn_5 = {
  async test() {
    const pattern = new URLPattern('/books/:id(\\d+)', 'https://example.com');
    assert.equal(pattern.exec('https://example.com/books/123').pathname.groups.id, "123");
  }
}

export const test_mdn_6 = {
  async test() {
    const pattern = new URLPattern('/books/(\\d+)', 'https://example.com');
    assert.equal(pattern.exec('https://example.com/books/123').pathname.groups["0"], "123");
  }
}

export const test_mdn_7 = {
  async test() {
    const pattern = new URLPattern('/books/:id?', 'https://example.com');
    assert.ok(pattern.test('https://example.com/books/123'));
    assert.ok(pattern.test('https://example.com/books'));
    assert.ok(!pattern.test('https://example.com/books/'));
    assert.ok(!pattern.test('https://example.com/books/123/456'));
    assert.ok(!pattern.test('https://example.com/books/123/456/789'));
  }
}

export const test_mdn_8 = {
  async test() {
    // A repeating group with a minimum of one
    const pattern = new URLPattern('/books/:id+', 'https://example.com');
    assert.ok(pattern.test('https://example.com/books/123'));
    assert.ok(!pattern.test('https://example.com/books'));
    assert.ok(!pattern.test('https://example.com/books/'));
    assert.ok(pattern.test('https://example.com/books/123/456'));
    assert.ok(pattern.test('https://example.com/books/123/456/789'));
  }
}

export const test_mdn_9 = {
  async test() {
    // A repeating group with a minimum of zero
    const pattern = new URLPattern('/books/:id*', 'https://example.com');
    assert.ok(pattern.test('https://example.com/books/123'));
    assert.ok(pattern.test('https://example.com/books'));
    assert.ok(!pattern.test('https://example.com/books/'));
    assert.ok(pattern.test('https://example.com/books/123/456'));
    assert.ok(pattern.test('https://example.com/books/123/456/789'));
  }
}

export const test_mdn_10 = {
  async test() {
    // A group delimiter with a ? (optional) modifier
    const pattern = new URLPattern('/book{s}?', 'https://example.com');
    assert.ok(pattern.test('https://example.com/books'));
    assert.ok(pattern.test('https://example.com/book'));
    assert.deepEqual(Object.keys(pattern.exec('https://example.com/books').pathname.groups), []);
  }
}

export const test_mdn_11 = {
  async test() {
    // A group delimiter without a modifier
    const pattern = new URLPattern('/book{s}', 'https://example.com');
    assert.equal(pattern.pathname, '/books');
    assert.ok(pattern.test('https://example.com/books'));
    assert.ok(!pattern.test('https://example.com/book'));
  }
}

export const test_mdn_12 = {
  async test() {
    // A group delimiter containing a capturing group
    const pattern = new URLPattern({ pathname: '/blog/:id(\\d+){-:title}?' });
    assert.ok(pattern.test('https://example.com/blog/123-my-blog'));
    assert.ok(pattern.test('https://example.com/blog/123'));
    assert.ok(!pattern.test('https://example.com/blog/my-blog'));
  }
}

export const test_mdn_13 = {
  async test() {
    // A pattern with an optional group, preceded by a slash
    const pattern = new URLPattern('/books/:id?', 'https://example.com');
    assert.ok(pattern.test('https://example.com/books/123'));
    assert.ok(pattern.test('https://example.com/books'));
    assert.ok(!pattern.test('https://example.com/books/'));
  }
}

export const test_mdn_14 = {
  async test() {
    // A pattern with a repeating group, preceded by a slash
    const pattern = new URLPattern('/books/:id+', 'https://example.com');
    assert.ok(pattern.test('https://example.com/books/123'));
    assert.ok(pattern.test('https://example.com/books/123/456'));
    assert.ok(!pattern.test('https://example.com/books/123/'));
    assert.ok(!pattern.test('https://example.com/books/123/456/'));
  }
}

export const test_mdn_15 = {
  async test() {
    // Segment prefixing does not occur outside of pathname patterns
    const pattern = new URLPattern({ hash: '/books/:id?' });
    assert.ok(pattern.test('https://example.com#/books/123'));
    assert.ok(!pattern.test('https://example.com#/books'));
    assert.ok(pattern.test('https://example.com#/books/'));
  }
}

export const test_mdn_16 = {
  async test() {
    // Disabling segment prefixing for a group using a group delimiter
    const pattern = new URLPattern({ pathname: '/books/{:id}?' });
    assert.ok(pattern.test('https://example.com/books/123'));
    assert.ok(!pattern.test('https://example.com/books'));
    assert.ok(pattern.test('https://example.com/books/'));
  }
}

export const test_mdn_17 = {
  async test() {
    // A wildcard at the end of a pattern
    const pattern = new URLPattern('/books/*', 'https://example.com');
    assert.ok(pattern.test('https://example.com/books/123'));
    assert.ok(!pattern.test('https://example.com/books'));
    assert.ok(pattern.test('https://example.com/books/'));
    assert.ok(pattern.test('https://example.com/books/123/456'));
  }
}


export const test_mdn_18 = {
  async test() {
    // A wildcard in the middle of a pattern
    const pattern = new URLPattern('/*.png', 'https://example.com');
    assert.ok(pattern.test('https://example.com/image.png'));
    assert.ok(!pattern.test('https://example.com/image.png/123'));
    assert.ok(pattern.test('https://example.com/folder/image.png'));
    assert.ok(pattern.test('https://example.com/.png'));
  }
}

export const test_mdn_19 = {
  async test() {
    // Construct a URLPattern that matches a specific domain and its subdomains.
    // All other URL components default to the wildcard `*` pattern.
    const pattern = new URLPattern({
      hostname: '{*.}?example.com',
    });
    assert.equal(pattern.hostname, "{*.}?example.com");
    assert.equal(pattern.protocol, "*");
    assert.equal(pattern.username, "*");
    assert.equal(pattern.password, "*");
    assert.equal(pattern.pathname, "*");
    assert.equal(pattern.search, "*");
    assert.equal(pattern.hash, "*");
    assert.ok(pattern.test('https://example.com/foo/bar'));
    assert.ok(pattern.test({ hostname: 'cdn.example.com' }));
    assert.ok(pattern.test('custom-protocol://example.com/other/path?q=1'));
    assert.ok(!pattern.test('https://cdn-example.com/foo/bar'));
  }
}

export const test_mdn_20 = {
  async test() {
    // Construct a URLPattern that matches URLs to CDN servers loading jpg images.
    // URL components not explicitly specified, like search and hash here, result
    // in the empty string similar to the URL() constructor.
    const pattern = new URLPattern('https://cdn-*.example.com/*.jpg');
    assert.equal(pattern.protocol, 'https');
    assert.equal(pattern.hostname, 'cdn-*.example.com');
    assert.equal(pattern.pathname, '/*.jpg');
    assert.equal(pattern.username, '');
    assert.equal(pattern.password, '');
    assert.equal(pattern.search, "");
    assert.equal(pattern.hash, "");
    assert.ok(pattern.test("https://cdn-1234.example.com/product/assets/hero.jpg"));
    assert.ok(!pattern.test("https://cdn-1234.example.com/product/assets/hero.jpg?q=1"));
  }
}

export const test_mdn_21 = {
  async test() {
    // Throws because this is interpreted as a single relative pathname pattern
    // with a ":foo" named group and there is no base URL.
    assert.throws(() => new URLPattern('data:foo*'), TypeError);
  }
}

export const test_mdn_22 = {
  async test() {
    const pattern = new URLPattern({ hostname: 'example.com', pathname: '/foo/*' });
    assert.ok(pattern.test({
      pathname: '/foo/bar',
      baseURL: 'https://example.com/baz',
    }));
    assert.ok(pattern.test('/foo/bar', 'https://example.com/baz'));
    // Throws because the second argument cannot be passed with a dictionary input.
    assert.throws(() => {
      pattern.test({ pathname: '/foo/bar' }, 'https://example.com/baz');
    }, TypeError);
    // The `exec()` method takes the same arguments as `test()`.
    const result = pattern.exec('/foo/bar', 'https://example.com/baz');
    assert.equal(result.pathname.input, '/foo/bar');
    assert.equal(result.pathname.groups[0], 'bar');
    assert.equal(result.hostname.input, 'example.com');
  }
}

export const test_mdn_23 = {
  async test() {
    const pattern1 = new URLPattern({ pathname: '/foo/*', baseURL: 'https://example.com' });
    assert.equal(pattern1.protocol, 'https');
    assert.equal(pattern1.hostname, 'example.com');
    assert.equal(pattern1.pathname, '/foo/*');
    assert.equal(pattern1.username, '');
    assert.equal(pattern1.password, '');
    assert.equal(pattern1.port, '');
    assert.equal(pattern1.search, '');
    assert.equal(pattern1.hash, '');
    new URLPattern('/foo/*', 'https://example.com');
    assert.throws(() => new URLPattern('/foo/*'), TypeError);
  }
}

export const test_mdn_24 = {
  async test() {
    const pattern = new URLPattern({ hostname: '*.example.com' });
    const result = pattern.exec({ hostname: 'cdn.example.com' });
    assert.equal(result.hostname.groups[0], 'cdn');
    assert.equal(result.hostname.input, 'cdn.example.com');
    assert.equal(result.inputs?.[0].hostname, 'cdn.example.com');
  }
}

export const test_mdn_25 = {
  async test() {
    // Construct a URLPattern using matching groups with custom names.  These
    // names can then be later used to access the matched values in the result
    // object.
    const pattern = new URLPattern({ pathname: '/:product/:user/:action' });
    const result = pattern.exec({ pathname: '/store/wanderview/view' });
    assert.equal(result.pathname.groups.product, 'store');
    assert.equal(result.pathname.groups.user, 'wanderview');
    assert.equal(result.pathname.groups.action, 'view');
    assert.equal(result.pathname.input, '/store/wanderview/view');
    assert.equal(result.inputs?.[0].pathname, '/store/wanderview/view');
  }
}

export const test_mdn_26 = {
  async test() {
    const pattern = new URLPattern({ pathname: '/product/:action+' });
    const result = pattern.exec({ pathname: '/product/do/some/thing/cool' });
    assert.equal(result.pathname.groups.action, 'do/some/thing/cool');
    assert.ok(!pattern.test({ pathname: '/product' }));
  }
}

export const test_mdn_27 = {
  async test() {
    const pattern = new URLPattern({ pathname: '/product/:action*' });
    const result = pattern.exec({ pathname: '/product/do/some/thing/cool' });
    assert.equal(result.pathname.groups.action, 'do/some/thing/cool');
    assert.ok(pattern.test({ pathname: '/product' }));
  }
}

export const test_mdn_28 = {
  async test() {
    const pattern = new URLPattern({ hostname: '{:subdomain.}*example.com' });
    assert.ok(pattern.test({ hostname: 'example.com' }));
    assert.ok(pattern.test({ hostname: 'foo.bar.example.com' }));
    assert.ok(!pattern.test({ hostname: '.example.com' }));
    const result = pattern.exec({ hostname: 'foo.bar.example.com' });
    assert.equal(result.hostname.groups.subdomain, 'foo.bar');
  }
}

export const test_mdn_29 = {
  async test() {
    const pattern = new URLPattern({ pathname: '/product{/}?' });
    assert.ok(pattern.test({ pathname: '/product' }));
    assert.ok(pattern.test({ pathname: '/product/' }));
    const result = pattern.exec({ pathname: '/product/' });
    assert.deepEqual(Object.keys(result.pathname.groups), []);
  }
}


export const test_mdn_32 = {
  async test() {
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

    assert.equal(result.username.groups.user, 'foo');
    assert.equal(result.password.groups.pass, 'bar');
    assert.equal(result.hostname.groups.subdomain, 'sub');
    assert.equal(result.pathname.groups.action, 'view');
  }
}

export const test_mdn_33 = {
  async test() {
    // Constructs a URLPattern treating the `:` as the protocol suffix.
    const pattern = new URLPattern('data\\:foo*');
    assert.equal(pattern.protocol, 'data');
    assert.equal(pattern.pathname, 'foo*');
    assert.equal(pattern.username, '');
    assert.equal(pattern.password, '');
    assert.equal(pattern.hostname, '');
    assert.equal(pattern.port, '');
    assert.equal(pattern.search, '');
    assert.equal(pattern.hash, '');
    assert.ok(pattern.test('data:foobar'));
  }
}

export const test_mdn_34 = {
  async test() {
    const pattern = new URLPattern({ pathname: '/(foo|bar)' });
    assert.ok(pattern.test({ pathname: '/foo' }));
    assert.ok(pattern.test({ pathname: '/bar' }));
    assert.ok(!pattern.test({ pathname: '/baz' }));
    const result = pattern.exec({ pathname: '/foo' });
    assert.equal(result.pathname.groups[0], 'foo');
  }
}

export const test_mdn_35 = {
  async test() {
    const pattern = new URLPattern({ pathname: '/product/(index.html)?' });
    assert.ok(pattern.test({ pathname: '/product/index.html' }));
    assert.ok(pattern.test({ pathname: '/product' }));
    const pattern2 = new URLPattern({ pathname: '/product/:action?' });
    assert.ok(pattern2.test({ pathname: '/product/view' }));
    assert.ok(pattern2.test({ pathname: '/product' }));
    // Wildcards can be made optional as well.  This may not seem to make sense
    // since they already match the empty string, but it also makes the prefix
    // `/` optional in a pathname pattern.
    const pattern3 = new URLPattern({ pathname: '/product/*?' });
    assert.ok(pattern3.test({ pathname: '/product/wanderview/view' }));
    assert.ok(pattern3.test({ pathname: '/product' }));
    assert.ok(pattern3.test({ pathname: '/product/' }));
  }
}

export const test_url_pattern_fun = {
  async test() {
    // The pattern is not useful but it should parse and produce the
    // same results we see in the browser.
    const pattern = new URLPattern(":café://:\u1234/:_✔️");
    assert.equal(pattern.protocol, ":café")
    assert.equal(pattern.hostname, ":ሴ");
    assert.equal(pattern.pathname, "/:_%E2%9C%94%EF%B8%8F");

    {
      // There was a bug that would cause a crash. Instead the following should throw
      // as invalid URLPattern syntax.
      assert.throws(() => new URLPattern({ hash: "=((" }), TypeError);
    }
  }
}

