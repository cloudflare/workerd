import {
  deepEqual,
  deepStrictEqual,
  doesNotMatch,
  doesNotReject,
  doesNotThrow,
  equal,
  fail,
  ifError,
  match,
  notDeepEqual,
  notDeepStrictEqual,
  notEqual,
  notStrictEqual,
  ok,
  rejects,
  strictEqual,
  throws,
} from 'node:assert';

import { default as path } from 'node:path';

export const test_path = {
  test(ctrl, env, ctx) {
    // Test thrown TypeErrors
    const typeErrorTests = [true, false, 7, null, {}, undefined, [], NaN];

    function fail(fn) {
      const args = Array.from(arguments).slice(1);

      throws(
        () => {
          fn.apply(null, args);
        },
        { code: 'ERR_INVALID_ARG_TYPE', name: 'TypeError' }
      );
    }

    typeErrorTests.forEach((test) => {
      [path.posix].forEach((namespace) => {
        fail(namespace.join, test);
        fail(namespace.resolve, test);
        fail(namespace.normalize, test);
        fail(namespace.isAbsolute, test);
        fail(namespace.relative, test, 'foo');
        fail(namespace.relative, 'foo', test);
        fail(namespace.parse, test);
        fail(namespace.dirname, test);
        fail(namespace.basename, test);
        fail(namespace.extname, test);

        // Undefined is a valid value as the second argument to basename
        if (test !== undefined) {
          fail(namespace.basename, 'foo', test);
        }
      });
    });

    // path.sep tests
    // windows
    strictEqual(path.win32.sep, '\\');
    // posix
    strictEqual(path.posix.sep, '/');

    // path.delimiter tests
    // windows
    strictEqual(path.win32.delimiter, ';');
    // posix
    strictEqual(path.posix.delimiter, ':');

    strictEqual(path, path.posix);
  },
};

export const test_path_zero_length_strings = {
  test(ctrl, env, ctx) {
    // Join will internally ignore all the zero-length strings and it will return
    // '.' if the joined string is a zero-length string.
    strictEqual(path.posix.join(''), '.');
    strictEqual(path.posix.join('', ''), '.');
    strictEqual(path.join('/'), '/');
    strictEqual(path.join('/', ''), '/');

    // Normalize will return '.' if the input is a zero-length string
    strictEqual(path.posix.normalize(''), '.');
    strictEqual(path.normalize('/'), '/');

    // Since '' is not a valid path in any of the common environments, return false
    strictEqual(path.posix.isAbsolute(''), false);

    // Resolve, internally ignores all the zero-length strings and returns the
    // current working directory
    strictEqual(path.resolve(''), '/');
    strictEqual(path.resolve('', ''), '/');

    // Relative, internally calls resolve. So, '' is actually the current directory
    strictEqual(path.relative('', '/'), '');
    strictEqual(path.relative('/', ''), '');
    strictEqual(path.relative('/', '/'), '');
  },
};

// Ref: https://github.com/nodejs/node/blob/4d6d7d644be4f10f90e5c9c66563736112fffbff/test/parallel/test-path-resolve.js
export const test_path_resolve = {
  test(ctrl, env, ctx) {
    const failures = [];
    const posixyCwd = '/';

    const resolveTests = [
      [['/var/lib', '../', 'file/'], '/var/file'],
      [['/var/lib', '/../', 'file/'], '/file'],
      [['a/b/c/', '../../..'], posixyCwd],
      [['.'], posixyCwd],
      [['/some/dir', '.', '/absolute/'], '/absolute'],
      [['/foo/tmp.3/', '../tmp.3/cycles/root.js'], '/foo/tmp.3/cycles/root.js'],
    ];
    resolveTests.forEach(([test, expected]) => {
      const actual = path.resolve.apply(null, test);
      const message = `path.posix.resolve(${test.map(JSON.stringify).join(',')})\n  expect=${JSON.stringify(
        expected
      )}\n  actual=${JSON.stringify(actual)}`;
      if (actual !== expected) failures.push(message);
    });
    strictEqual(failures.length, 0, failures.join('\n'));
  },
};

// Ref: https://github.com/nodejs/node/blob/4d6d7d644be4f10f90e5c9c66563736112fffbff/test/parallel/test-path-relative.js
export const test_path_relative = {
  test(ctrl, env, ctx) {
    const failures = [];

    const relativeTests = [
      ['/var/lib', '/var', '..'],
      ['/var/lib', '/bin', '../../bin'],
      ['/var/lib', '/var/lib', ''],
      ['/var/lib', '/var/apache', '../apache'],
      ['/var/', '/var/lib', 'lib'],
      ['/', '/var/lib', 'var/lib'],
      ['/foo/test', '/foo/test/bar/package.json', 'bar/package.json'],
      ['/Users/a/web/b/test/mails', '/Users/a/web/b', '../..'],
      ['/foo/bar/baz-quux', '/foo/bar/baz', '../baz'],
      ['/foo/bar/baz', '/foo/bar/baz-quux', '../baz-quux'],
      ['/baz-quux', '/baz', '../baz'],
      ['/baz', '/baz-quux', '../baz-quux'],
      ['/page1/page2/foo', '/', '../../..'],
    ];
    relativeTests.forEach((test) => {
      const actual = path.relative(test[0], test[1]);
      const expected = test[2];
      if (actual !== expected) {
        const message = `path.posix.relative(${test
          .slice(0, 2)
          .map(JSON.stringify)
          .join(',')})\n  expect=${JSON.stringify(
          expected
        )}\n  actual=${JSON.stringify(actual)}`;
        failures.push(`\n${message}`);
      }
    });
    strictEqual(failures.length, 0, failures.join(''));
  },
};

export const test_path_parse_format = {
  test(ctrl, env, ctx) {
    const unixPaths = [
      // [path, root]
      ['/home/user/dir/file.txt', '/'],
      ['/home/user/a dir/another File.zip', '/'],
      ['/home/user/a dir//another&File.', '/'],
      ['/home/user/a$$$dir//another File.zip', '/'],
      ['user/dir/another File.zip', ''],
      ['file', ''],
      ['.\\file', ''],
      ['./file', ''],
      ['C:\\foo', ''],
      ['/', '/'],
      ['', ''],
      ['.', ''],
      ['..', ''],
      ['/foo', '/'],
      ['/foo.', '/'],
      ['/foo.bar', '/'],
      ['/.', '/'],
      ['/.foo', '/'],
      ['/.foo.bar', '/'],
      ['/foo/bar.baz', '/'],
    ];

    const unixSpecialCaseFormatTests = [
      [{ dir: 'some/dir' }, 'some/dir/'],
      [{ base: 'index.html' }, 'index.html'],
      [{ root: '/' }, '/'],
      [{ name: 'index', ext: '.html' }, 'index.html'],
      [{ dir: 'some/dir', name: 'index', ext: '.html' }, 'some/dir/index.html'],
      [{ root: '/', name: 'index', ext: '.html' }, '/index.html'],
      [{}, ''],
    ];

    const errors = [
      { method: 'parse', input: [null] },
      { method: 'parse', input: [{}] },
      { method: 'parse', input: [true] },
      { method: 'parse', input: [1] },
      { method: 'parse', input: [] },
      { method: 'format', input: [null] },
      { method: 'format', input: [''] },
      { method: 'format', input: [true] },
      { method: 'format', input: [1] },
    ];

    checkParseFormat(unixPaths);
    checkErrors();
    checkFormat(unixSpecialCaseFormatTests);

    // Test removal of trailing path separators
    const trailingTests = [
      ['./', { root: '', dir: '', base: '.', ext: '', name: '.' }],
      ['//', { root: '/', dir: '/', base: '', ext: '', name: '' }],
      ['///', { root: '/', dir: '/', base: '', ext: '', name: '' }],
      ['/foo///', { root: '/', dir: '/', base: 'foo', ext: '', name: 'foo' }],
      [
        '/foo///bar.baz',
        { root: '/', dir: '/foo//', base: 'bar.baz', ext: '.baz', name: 'bar' },
      ],
    ];
    const failures = [];
    trailingTests.forEach((test) => {
      const actual = path.parse(test[0]);
      const expected = test[1];
      const message = `path.posix.parse(${JSON.stringify(test[0])})\n  expect=${JSON.stringify(
        expected
      )}\n  actual=${JSON.stringify(actual)}`;
      const actualKeys = Object.keys(actual);
      const expectedKeys = Object.keys(expected);
      let failed = actualKeys.length !== expectedKeys.length;
      if (!failed) {
        for (let i = 0; i < actualKeys.length; ++i) {
          const key = actualKeys[i];
          if (!expectedKeys.includes(key) || actual[key] !== expected[key]) {
            failed = true;
            break;
          }
        }
      }
      if (failed) failures.push(`\n${message}`);
    });
    strictEqual(failures.length, 0, failures.join(''));

    function checkErrors() {
      errors.forEach(({ method, input }) => {
        throws(
          () => {
            path[method].apply(path, input);
          },
          {
            code: 'ERR_INVALID_ARG_TYPE',
            name: 'TypeError',
          }
        );
      });
    }

    function checkParseFormat(paths) {
      paths.forEach(([element, root]) => {
        const output = path.parse(element);
        strictEqual(typeof output.root, 'string');
        strictEqual(typeof output.dir, 'string');
        strictEqual(typeof output.base, 'string');
        strictEqual(typeof output.ext, 'string');
        strictEqual(typeof output.name, 'string');
        strictEqual(path.format(output), element);
        strictEqual(output.root, root);
        ok(output.dir.startsWith(output.root));
        strictEqual(output.dir, output.dir ? path.dirname(element) : '');
        strictEqual(output.base, path.basename(element));
        strictEqual(output.ext, path.extname(element));
      });
    }

    function checkFormat(testCases) {
      testCases.forEach(([element, expect]) => {
        strictEqual(path.format(element), expect);
      });

      [null, undefined, 1, true, false, 'string'].forEach((pathObject) => {
        throws(
          () => {
            path.format(pathObject);
          },
          {
            code: 'ERR_INVALID_ARG_TYPE',
            name: 'TypeError',
          }
        );
      });
    }

    // See https://github.com/nodejs/node/issues/44343
    strictEqual(path.format({ name: 'x', ext: 'png' }), 'x.png');
    strictEqual(path.format({ name: 'x', ext: '.png' }), 'x.png');
  },
};

export const test_path_normalize = {
  test(ctrl, env, ctx) {
    strictEqual(
      path.win32.normalize('./fixtures///b/../b/c.js'),
      'fixtures\\b\\c.js'
    );
    strictEqual(path.win32.normalize('/foo/../../../bar'), '\\bar');
    strictEqual(path.win32.normalize('a//b//../b'), 'a\\b');
    strictEqual(path.win32.normalize('a//b//./c'), 'a\\b\\c');
    strictEqual(path.win32.normalize('a//b//.'), 'a\\b');
    strictEqual(
      path.win32.normalize('//server/share/dir/file.ext'),
      '\\\\server\\share\\dir\\file.ext'
    );
    strictEqual(path.win32.normalize('/a/b/c/../../../x/y/z'), '\\x\\y\\z');
    strictEqual(path.win32.normalize('C:'), 'C:.');
    strictEqual(path.win32.normalize('C:..\\abc'), 'C:..\\abc');
    strictEqual(
      path.win32.normalize('C:..\\..\\abc\\..\\def'),
      'C:..\\..\\def'
    );
    strictEqual(path.win32.normalize('C:\\.'), 'C:\\');
    strictEqual(path.win32.normalize('file:stream'), 'file:stream');
    strictEqual(path.win32.normalize('bar\\foo..\\..\\'), 'bar\\');
    strictEqual(path.win32.normalize('bar\\foo..\\..'), 'bar');
    strictEqual(path.win32.normalize('bar\\foo..\\..\\baz'), 'bar\\baz');
    strictEqual(path.win32.normalize('bar\\foo..\\'), 'bar\\foo..\\');
    strictEqual(path.win32.normalize('bar\\foo..'), 'bar\\foo..');
    strictEqual(path.win32.normalize('..\\foo..\\..\\..\\bar'), '..\\..\\bar');
    strictEqual(
      path.win32.normalize('..\\...\\..\\.\\...\\..\\..\\bar'),
      '..\\..\\bar'
    );
    strictEqual(
      path.win32.normalize('../../../foo/../../../bar'),
      '..\\..\\..\\..\\..\\bar'
    );
    strictEqual(
      path.win32.normalize('../../../foo/../../../bar/../../'),
      '..\\..\\..\\..\\..\\..\\'
    );
    strictEqual(
      path.win32.normalize('../foobar/barfoo/foo/../../../bar/../../'),
      '..\\..\\'
    );
    strictEqual(
      path.win32.normalize('../.../../foobar/../../../bar/../../baz'),
      '..\\..\\..\\..\\baz'
    );
    strictEqual(path.win32.normalize('foo/bar\\baz'), 'foo\\bar\\baz');

    strictEqual(
      path.posix.normalize('./fixtures///b/../b/c.js'),
      'fixtures/b/c.js'
    );
    strictEqual(path.posix.normalize('/foo/../../../bar'), '/bar');
    strictEqual(path.posix.normalize('a//b//../b'), 'a/b');
    strictEqual(path.posix.normalize('a//b//./c'), 'a/b/c');
    strictEqual(path.posix.normalize('a//b//.'), 'a/b');
    strictEqual(path.posix.normalize('/a/b/c/../../../x/y/z'), '/x/y/z');
    strictEqual(path.posix.normalize('///..//./foo/.//bar'), '/foo/bar');
    strictEqual(path.posix.normalize('bar/foo../../'), 'bar/');
    strictEqual(path.posix.normalize('bar/foo../..'), 'bar');
    strictEqual(path.posix.normalize('bar/foo../../baz'), 'bar/baz');
    strictEqual(path.posix.normalize('bar/foo../'), 'bar/foo../');
    strictEqual(path.posix.normalize('bar/foo..'), 'bar/foo..');
    strictEqual(path.posix.normalize('../foo../../../bar'), '../../bar');
    strictEqual(path.posix.normalize('../.../.././.../../../bar'), '../../bar');
    strictEqual(
      path.posix.normalize('../../../foo/../../../bar'),
      '../../../../../bar'
    );
    strictEqual(
      path.posix.normalize('../../../foo/../../../bar/../../'),
      '../../../../../../'
    );
    strictEqual(
      path.posix.normalize('../foobar/barfoo/foo/../../../bar/../../'),
      '../../'
    );
    strictEqual(
      path.posix.normalize('../.../../foobar/../../../bar/../../baz'),
      '../../../../baz'
    );
    strictEqual(path.posix.normalize('foo/bar\\baz'), 'foo/bar\\baz');
  },
};

export const test_path_join = {
  test(ctrl, env, ctx) {
    const failures = [];
    const backslashRE = /\\/g;

    const joinTests = [
      [['.', 'x/b', '..', '/b/c.js'], 'x/b/c.js'],
      [[], '.'],
      [['/.', 'x/b', '..', '/b/c.js'], '/x/b/c.js'],
      [['/foo', '../../../bar'], '/bar'],
      [['foo', '../../../bar'], '../../bar'],
      [['foo/', '../../../bar'], '../../bar'],
      [['foo/x', '../../../bar'], '../bar'],
      [['foo/x', './bar'], 'foo/x/bar'],
      [['foo/x/', './bar'], 'foo/x/bar'],
      [['foo/x/', '.', 'bar'], 'foo/x/bar'],
      [['./'], './'],
      [['.', './'], './'],
      [['.', '.', '.'], '.'],
      [['.', './', '.'], '.'],
      [['.', '/./', '.'], '.'],
      [['.', '/////./', '.'], '.'],
      [['.'], '.'],
      [['', '.'], '.'],
      [['', 'foo'], 'foo'],
      [['foo', '/bar'], 'foo/bar'],
      [['', '/foo'], '/foo'],
      [['', '', '/foo'], '/foo'],
      [['', '', 'foo'], 'foo'],
      [['foo', ''], 'foo'],
      [['foo/', ''], 'foo/'],
      [['foo', '', '/bar'], 'foo/bar'],
      [['./', '..', '/foo'], '../foo'],
      [['./', '..', '..', '/foo'], '../../foo'],
      [['.', '..', '..', '/foo'], '../../foo'],
      [['', '..', '..', '/foo'], '../../foo'],
      [['/'], '/'],
      [['/', '.'], '/'],
      [['/', '..'], '/'],
      [['/', '..', '..'], '/'],
      [[''], '.'],
      [['', ''], '.'],
      [[' /foo'], ' /foo'],
      [[' ', 'foo'], ' /foo'],
      [[' ', '.'], ' '],
      [[' ', '/'], ' /'],
      [[' ', ''], ' '],
      [['/', 'foo'], '/foo'],
      [['/', '/foo'], '/foo'],
      [['/', '//foo'], '/foo'],
      [['/', '', '/foo'], '/foo'],
      [['', '/', 'foo'], '/foo'],
      [['', '/', '/foo'], '/foo'],
    ];

    joinTests.forEach((test) => {
      const actual = path.join.apply(null, test[0]);
      const expected = test[1];
      if (actual !== expected && actualAlt !== expected) {
        const delimiter = test[0].map(JSON.stringify).join(',');
        const message = `path.posix.join(${delimiter})\n  expect=${JSON.stringify(
          expected
        )}\n  actual=${JSON.stringify(actual)}`;
        failures.push(`\n${message}`);
      }
    });
    strictEqual(failures.length, 0, failures.join(''));
  },
};

export const test_path_isabsolute = {
  test(ctrl, env, ctx) {
    strictEqual(path.posix.isAbsolute('/home/foo'), true);
    strictEqual(path.posix.isAbsolute('/home/foo/..'), true);
    strictEqual(path.posix.isAbsolute('bar/'), false);
    strictEqual(path.posix.isAbsolute('./baz'), false);
    strictEqual(path.isAbsolute('/home/foo'), true);
    strictEqual(path.isAbsolute('/home/foo/..'), true);
    strictEqual(path.isAbsolute('bar/'), false);
    strictEqual(path.isAbsolute('./baz'), false);
  },
};

export const test_path_extname = {
  test(ctrl, env, ctx) {
    const failures = [];
    const slashRE = /\//g;

    [
      ['', ''],
      ['/path/to/file', ''],
      ['/path/to/file.ext', '.ext'],
      ['/path.to/file.ext', '.ext'],
      ['/path.to/file', ''],
      ['/path.to/.file', ''],
      ['/path.to/.file.ext', '.ext'],
      ['/path/to/f.ext', '.ext'],
      ['/path/to/..ext', '.ext'],
      ['/path/to/..', ''],
      ['file', ''],
      ['file.ext', '.ext'],
      ['.file', ''],
      ['.file.ext', '.ext'],
      ['/file', ''],
      ['/file.ext', '.ext'],
      ['/.file', ''],
      ['/.file.ext', '.ext'],
      ['.path/file.ext', '.ext'],
      ['file.ext.ext', '.ext'],
      ['file.', '.'],
      ['.', ''],
      ['./', ''],
      ['.file.ext', '.ext'],
      ['.file', ''],
      ['.file.', '.'],
      ['.file..', '.'],
      ['..', ''],
      ['../', ''],
      ['..file.ext', '.ext'],
      ['..file', '.file'],
      ['..file.', '.'],
      ['..file..', '.'],
      ['...', '.'],
      ['...ext', '.ext'],
      ['....', '.'],
      ['file.ext/', '.ext'],
      ['file.ext//', '.ext'],
      ['file/', ''],
      ['file//', ''],
      ['file./', '.'],
      ['file.//', '.'],
    ].forEach((test) => {
      const expected = test[1];
      const actual = path.extname(test[0]);
      const message = `path.posix.extname(${JSON.stringify(test[0])})\n  expect=${JSON.stringify(
        expected
      )}\n  actual=${JSON.stringify(actual)}`;
      if (actual !== expected) failures.push(`\n${message}`);
    });

    strictEqual(failures.length, 0, failures.join(''));

    // On *nix, backslash is a valid name component like any other character.
    strictEqual(path.posix.extname('.\\'), '');
    strictEqual(path.posix.extname('..\\'), '.\\');
    strictEqual(path.posix.extname('file.ext\\'), '.ext\\');
    strictEqual(path.posix.extname('file.ext\\\\'), '.ext\\\\');
    strictEqual(path.posix.extname('file\\'), '');
    strictEqual(path.posix.extname('file\\\\'), '');
    strictEqual(path.posix.extname('file.\\'), '.\\');
    strictEqual(path.posix.extname('file.\\\\'), '.\\\\');
  },
};

export const test_path_dirname = {
  test(ctrl, env, ctx) {
    strictEqual(path.posix.dirname('/a/b/'), '/a');
    strictEqual(path.posix.dirname('/a/b'), '/a');
    strictEqual(path.posix.dirname('/a'), '/');
    strictEqual(path.posix.dirname(''), '.');
    strictEqual(path.posix.dirname('/'), '/');
    strictEqual(path.posix.dirname('////'), '/');
    strictEqual(path.posix.dirname('//a'), '//');
    strictEqual(path.posix.dirname('foo'), '.');
    strictEqual(path.dirname('/a/b/'), '/a');
    strictEqual(path.dirname('/a/b'), '/a');
    strictEqual(path.dirname('/a'), '/');
    strictEqual(path.dirname(''), '.');
    strictEqual(path.dirname('/'), '/');
    strictEqual(path.dirname('////'), '/');
    strictEqual(path.dirname('//a'), '//');
    strictEqual(path.dirname('foo'), '.');
  },
};

export const test_path_basename = {
  test(ctrl, env, ctx) {
    strictEqual(path.basename('.js', '.js'), '');
    strictEqual(path.basename('js', '.js'), 'js');
    strictEqual(path.basename('file.js', '.ts'), 'file.js');
    strictEqual(path.basename('file', '.js'), 'file');
    strictEqual(path.basename('file.js.old', '.js.old'), 'file');
    strictEqual(path.basename(''), '');
    strictEqual(path.basename('/dir/basename.ext'), 'basename.ext');
    strictEqual(path.basename('/basename.ext'), 'basename.ext');
    strictEqual(path.basename('basename.ext'), 'basename.ext');
    strictEqual(path.basename('basename.ext/'), 'basename.ext');
    strictEqual(path.basename('basename.ext//'), 'basename.ext');
    strictEqual(path.basename('aaa/bbb', '/bbb'), 'bbb');
    strictEqual(path.basename('aaa/bbb', 'a/bbb'), 'bbb');
    strictEqual(path.basename('aaa/bbb', 'bbb'), 'bbb');
    strictEqual(path.basename('aaa/bbb//', 'bbb'), 'bbb');
    strictEqual(path.basename('aaa/bbb', 'bb'), 'b');
    strictEqual(path.basename('aaa/bbb', 'b'), 'bb');
    strictEqual(path.basename('/aaa/bbb', '/bbb'), 'bbb');
    strictEqual(path.basename('/aaa/bbb', 'a/bbb'), 'bbb');
    strictEqual(path.basename('/aaa/bbb', 'bbb'), 'bbb');
    strictEqual(path.basename('/aaa/bbb//', 'bbb'), 'bbb');
    strictEqual(path.basename('/aaa/bbb', 'bb'), 'b');
    strictEqual(path.basename('/aaa/bbb', 'b'), 'bb');
    strictEqual(path.basename('/aaa/bbb'), 'bbb');
    strictEqual(path.basename('/aaa/'), 'aaa');
    strictEqual(path.basename('/aaa/b'), 'b');
    strictEqual(path.basename('/a/b'), 'b');
    strictEqual(path.basename('//a'), 'a');
    strictEqual(path.basename('a', 'a'), '');

    // On unix a backslash is just treated as any other character.
    strictEqual(
      path.posix.basename('\\dir\\basename.ext'),
      '\\dir\\basename.ext'
    );
    strictEqual(path.posix.basename('\\basename.ext'), '\\basename.ext');
    strictEqual(path.posix.basename('basename.ext'), 'basename.ext');
    strictEqual(path.posix.basename('basename.ext\\'), 'basename.ext\\');
    strictEqual(path.posix.basename('basename.ext\\\\'), 'basename.ext\\\\');
    strictEqual(path.posix.basename('foo'), 'foo');

    // POSIX filenames may include control characters
    // c.f. http://www.dwheeler.com/essays/fixing-unix-linux-filenames.html
    const controlCharFilename = `Icon${String.fromCharCode(13)}`;
    strictEqual(
      path.posix.basename(`/a/b/${controlCharFilename}`),
      controlCharFilename
    );
  },
};
