// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import { glob, globSync, mkdirSync, writeFileSync, promises } from 'node:fs';

// Node.js test fixture structure (minus symlinks — not supported in workerd VFS tests)
function setupFixtures() {
  const base = '/tmp/gt';
  mkdirSync(base + '/a/.abcdef/x/y/z', { recursive: true });
  mkdirSync(base + '/a/abcdef/g', { recursive: true });
  mkdirSync(base + '/a/abcfed/g', { recursive: true });
  mkdirSync(base + '/a/b/c', { recursive: true });
  mkdirSync(base + '/a/bc/e', { recursive: true });
  mkdirSync(base + '/a/c/d/c', { recursive: true });
  mkdirSync(base + '/a/cb/e', { recursive: true });
  mkdirSync(base + '/a/x/.y', { recursive: true });
  mkdirSync(base + '/a/z/.y', { recursive: true });

  writeFileSync(base + '/a/.abcdef/x/y/z/a', 'x');
  writeFileSync(base + '/a/abcdef/g/h', 'x');
  writeFileSync(base + '/a/abcfed/g/h', 'x');
  writeFileSync(base + '/a/b/c/d', 'x');
  writeFileSync(base + '/a/bc/e/f', 'x');
  writeFileSync(base + '/a/c/d/c/b', 'x');
  writeFileSync(base + '/a/cb/e/f', 'x');
  writeFileSync(base + '/a/x/.y/b', 'x');
  writeFileSync(base + '/a/z/.y/b', 'x');
  writeFileSync(base + '/a/.b', 'x');
  writeFileSync(base + '/a/b/.b', 'x');

  return base;
}

const cwd = '/tmp/gt';

// ============================================================================
// Pattern Matching Tests (ported from Node.js)
// ============================================================================

export const singleWildcardInSubdir = {
  test() {
    setupFixtures();
    const result = globSync('a/c/d/*/b', { cwd });
    deepStrictEqual(result.sort(), ['a/c/d/c/b']);
  },
};

export const doubleSlashNormalization = {
  test() {
    setupFixtures();
    // Double slashes should be normalized
    const result = globSync('a//c//d//*//b', { cwd });
    deepStrictEqual(result.sort(), ['a/c/d/c/b']);
  },
};

export const multipleWildcards = {
  test() {
    setupFixtures();
    const result = globSync('a/*/d/*/b', { cwd });
    deepStrictEqual(result.sort(), ['a/c/d/c/b']);
  },
};

export const extglobPlus = {
  test() {
    setupFixtures();
    // +(c|g) should match one or more occurrences of c or g
    // a/*/+(c|g)/./d → matches a/b/c/./d which resolves to a/b/c/d
    const result = globSync('a/*/+(c|g)/./d', { cwd });
    deepStrictEqual(result.sort(), ['a/b/c/d']);
  },
};

export const braceExpansion = {
  test() {
    setupFixtures();
    const result = globSync('a/abc{fed,def}/g/h', { cwd });
    deepStrictEqual(result.sort(), ['a/abcdef/g/h', 'a/abcfed/g/h']);
  },
};

export const braceExpansionNoMatch = {
  test() {
    setupFixtures();
    // None of b,c,d,e,f directories have a **/g path
    const result = globSync('a/{b,c,d,e,f}/**/g', { cwd });
    deepStrictEqual(result, []);
  },
};

export const globstarMatchesAll = {
  test() {
    setupFixtures();
    // a/b/** should match a/b itself and everything under it
    const result = globSync('a/b/**', { cwd });
    deepStrictEqual(result.sort(), ['a/b', 'a/b/.b', 'a/b/c', 'a/b/c/d']);
  },
};

export const dotSlashGlobstar = {
  test() {
    setupFixtures();
    // ./**/g should find all 'g' entries
    const result = globSync('./**/g', { cwd });
    deepStrictEqual(result.sort(), ['a/abcdef/g', 'a/abcfed/g']);
  },
};

export const globstarA = {
  test() {
    setupFixtures();
    // **/a — find all entries named 'a'
    const result = globSync('**/a', { cwd });
    ok(result.includes('a'));
    ok(result.includes('a/.abcdef/x/y/z/a'));
  },
};

export const threeWildcardLevelsF = {
  test() {
    setupFixtures();
    const result = globSync('*/*/*/f', { cwd });
    deepStrictEqual(result.sort(), ['a/bc/e/f', 'a/cb/e/f']);
  },
};

export const dotSlashGlobstarF = {
  test() {
    setupFixtures();
    const result = globSync('./**/f', { cwd });
    deepStrictEqual(result.sort(), ['a/bc/e/f', 'a/cb/e/f']);
  },
};

export const dotFileGlobstar = {
  test() {
    setupFixtures();
    // **/.b should match dot files named .b
    const result = globSync('**/.b', { cwd });
    deepStrictEqual(result.sort(), ['a/.b', 'a/b/.b']);
  },
};

export const globstarCharClass = {
  test() {
    setupFixtures();
    // a/**/[cg] — match entries named 'c' or 'g' at any depth under a/
    const result = globSync('a/**/[cg]', { cwd });
    ok(result.includes('a/abcdef/g'));
    ok(result.includes('a/abcfed/g'));
    ok(result.includes('a/b/c'));
    ok(result.includes('a/c'));
    ok(result.includes('a/c/d/c'));
  },
};

export const parentTraversal = {
  test() {
    setupFixtures();
    // a/**/[cg]/../[cg] — go into [cg] dir, go up, match [cg] again
    const result = globSync('a/**/[cg]/../[cg]', { cwd });
    ok(result.length > 0);
    // Should find paths where a directory matching [cg] has a sibling matching [cg]
    ok(result.includes('a/c'));
  },
};

export const extglobNegation = {
  test() {
    setupFixtures();
    // a/!(doesnotexist)/** — match all under a/ except entries named 'doesnotexist'
    const result = globSync('a/!(doesnotexist)/**', { cwd });
    ok(result.length > 0);
    ok(result.includes('a/b'));
    ok(result.includes('a/b/c'));
    ok(result.includes('a/b/c/d'));
  },
};

// ============================================================================
// Exclude Tests
// ============================================================================

export const excludeFunction = {
  test() {
    setupFixtures();
    // Exclude everything — should return empty
    const result = globSync('a/**', { cwd, exclude: () => true });
    deepStrictEqual(result, []);
  },
};

export const excludeFunctionSelective = {
  test() {
    setupFixtures();
    // Exclude paths containing 'abcdef'
    const result = globSync('a/abc*/g/h', {
      cwd,
      exclude: (p) => p.includes('abcdef'),
    });
    deepStrictEqual(result, ['a/abcfed/g/h']);
  },
};

export const excludeArrayPattern = {
  test() {
    setupFixtures();
    // Exclude using array of glob patterns
    const result = globSync('a/abc*/g/h', {
      cwd,
      exclude: ['**/abcdef/**'],
    });
    deepStrictEqual(result, ['a/abcfed/g/h']);
  },
};

export const excludeArrayMatchesAll = {
  test() {
    setupFixtures();
    // Exclude all with wildcard
    const result = globSync('a/**', { cwd, exclude: ['**'] });
    deepStrictEqual(result, []);
  },
};

// ============================================================================
// withFileTypes Tests
// ============================================================================

export const withFileTypesTest = {
  test() {
    setupFixtures();
    const result = globSync('a/b/c/d', { cwd, withFileTypes: true });
    strictEqual(result.length, 1);
    const dirent = result[0];
    strictEqual(dirent.name, 'd');
    ok(dirent.isFile());
    ok(!dirent.isDirectory());
  },
};

export const withFileTypesDirTest = {
  test() {
    setupFixtures();
    const result = globSync('a/b/c', { cwd, withFileTypes: true });
    strictEqual(result.length, 1);
    const dirent = result[0];
    strictEqual(dirent.name, 'c');
    ok(dirent.isDirectory());
    ok(!dirent.isFile());
  },
};

// ============================================================================
// Callback API Tests
// ============================================================================

export const callbackGlobTest = {
  async test() {
    setupFixtures();
    const result = await new Promise((resolve, reject) => {
      glob('a/abc{fed,def}/g/h', { cwd }, (err, matches) => {
        if (err) reject(err);
        else resolve(matches);
      });
    });
    deepStrictEqual([...result].sort(), ['a/abcdef/g/h', 'a/abcfed/g/h']);
  },
};

export const callbackGlobNoOptions = {
  async test() {
    const result = await new Promise((resolve, reject) => {
      glob('*.nonexistent', (err, matches) => {
        if (err) reject(err);
        else resolve(matches);
      });
    });
    deepStrictEqual(result, []);
  },
};

// ============================================================================
// Promises API Tests
// ============================================================================

export const promisesGlobTest = {
  async test() {
    setupFixtures();
    const results = [];
    for await (const entry of promises.glob('a/abc{fed,def}/g/h', { cwd })) {
      results.push(entry);
    }
    deepStrictEqual(results.sort(), ['a/abcdef/g/h', 'a/abcfed/g/h']);
  },
};

export const promisesGlobstarTest = {
  async test() {
    setupFixtures();
    const results = [];
    for await (const entry of promises.glob('**/.b', { cwd })) {
      results.push(entry);
    }
    deepStrictEqual(results.sort(), ['a/.b', 'a/b/.b']);
  },
};

// ============================================================================
// Edge Cases
// ============================================================================

export const noMatchTest = {
  test() {
    setupFixtures();
    const result = globSync('nonexistent/**', { cwd });
    deepStrictEqual(result, []);
  },
};

export const exactPathTest = {
  test() {
    setupFixtures();
    // Exact path match (no wildcards)
    const result = globSync('a/b/c/d', { cwd });
    deepStrictEqual(result, ['a/b/c/d']);
  },
};

export const defaultCwdTest = {
  test() {
    // Without cwd option, should use process.cwd() and not throw
    const result = globSync('*.nonexistent');
    deepStrictEqual(result, []);
  },
};

export const multiplePatternsTest = {
  test() {
    setupFixtures();
    const result = globSync(['a/b/c/d', 'a/bc/e/f'], { cwd });
    deepStrictEqual(result.sort(), ['a/b/c/d', 'a/bc/e/f']);
  },
};

export const braceWithGlobstar = {
  test() {
    setupFixtures();
    // a/{b/**,b/c} should match a/b and everything under it, plus a/b/c
    const result = globSync('a/{b/**,b/c}', { cwd });
    ok(result.includes('a/b'));
    ok(result.includes('a/b/c'));
    ok(result.includes('a/b/c/d'));
  },
};
