import {
  writeFileSync,
  mkdirSync,
  symlinkSync,
  cpSync,
  cp,
  existsSync,
  readFileSync,
  lstatSync,
  statSync,
  promises as fsPromises,
} from 'node:fs';

import { ok, deepStrictEqual, strictEqual, throws, rejects } from 'node:assert';

const pathA = new URL('file:///tmp/a'); // File
const pathB = new URL('file:///tmp/b'); // Directory
const pathBE = new URL('file:///tmp/b/e'); // File in directory
const pathC = new URL('file:///tmp/c'); // Symlink to file
const pathD = new URL('file:///tmp/d'); // Symlink to directory
const pathE = new URL('file:///tmp/e'); // Directory
const pathF = new URL('file:///tmp/f'); // Path to copy to

function setupPaths() {
  writeFileSync(pathA, 'foo');
  mkdirSync(pathB);
  symlinkSync(pathA, pathC);
  symlinkSync(pathB, pathD);
  mkdirSync(pathE);
  writeFileSync(pathBE, 'bar');
}

export const simpleFileCopy = {
  test() {
    setupPaths();
    ok(existsSync(pathA));
    deepStrictEqual(readFileSync(pathA, 'utf8'), 'foo');
    ok(!existsSync(pathF));
    cpSync(pathA, pathF);
    ok(existsSync(pathF));
    deepStrictEqual(readFileSync(pathF, 'utf8'), 'foo');
    writeFileSync(pathA, 'baz');
    deepStrictEqual(readFileSync(pathA, 'utf8'), 'baz');
    deepStrictEqual(readFileSync(pathF, 'utf8'), 'foo');
    cpSync(pathA, pathF);
    cpSync(pathA, pathF, { errorOnExist: true }); // force is true by default, weird but true.

    // Copying a file when the destination already exists and force is false and
    // errorOnExist is true, should throw an error.
    throws(() => cpSync(pathA, pathF, { force: false, errorOnExist: true }), {
      code: 'ERR_FS_CP_EEXIST',
    });

    // Copying a file to a directory should throw an error.
    throws(() => cpSync(pathA, pathB), {
      code: 'ERR_FS_CP_NON_DIR_TO_DIR',
    });

    // Copy a file to a symlink to a directory should throw by default
    throws(() => cpSync(pathA, pathD, { dereference: true }), {
      code: 'ERR_FS_CP_NON_DIR_TO_DIR',
    });

    ok(statSync(pathD).isDirectory());
    ok(lstatSync(pathD).isSymbolicLink());
    cpSync(pathA, pathD);
    ok(lstatSync(pathD).isFile());
  },
};

// Copy file to non-existent directory (should fail)
export const copyFileToNonExistentDirectory = {
  test() {
    setupPaths();
    // The directory will be created by cpSync if it doesn't exist.
    const pathG = new URL('file:///tmp/nonexistent');
    const pathH = new URL('file:///tmp/nonexistent/g');
    ok(!existsSync(pathG));
    ok(!existsSync(pathH));
    cpSync(pathA, pathH);
    ok(existsSync(pathG));
    ok(existsSync(pathH));
  },
};

// Copy file to existing file with force
export const copyFileToExistingFileWithForce = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    writeFileSync(pathG, 'original');
    cpSync(pathA, pathG, { force: true });
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'foo');
  },
};

// Copy file to existing file without force
export const copyFileToExistingFileWithoutForce = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    writeFileSync(pathG, 'original');
    // When force is false, it should not overwrite the existing file.
    cpSync(pathA, pathG, { force: false });
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'original');
  },
};

// Copy file to existing directory
export const copyFileToExistingDirectory = {
  test() {
    setupPaths();
    throws(() => cpSync(pathA, pathB), {
      code: 'ERR_FS_CP_NON_DIR_TO_DIR',
    });
  },
};

// Copy file to existing symlink to directory
export const copyFileToExistingSymlinkToDirectory = {
  test() {
    setupPaths();
    // With dereference: true, should throw EISDIR
    throws(() => cpSync(pathA, pathD, { dereference: true }), {
      code: 'ERR_FS_CP_NON_DIR_TO_DIR',
    });

    // Without dereference (default), should replace the symlink
    cpSync(pathA, pathD);
    ok(lstatSync(pathD).isFile());
    deepStrictEqual(readFileSync(pathD, 'utf8'), 'foo');
  },
};

// Copy directory to non-existent directory
export const copyDirectoryToNonExistentDirectory = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    cpSync(pathB, pathG, { recursive: true });
    ok(existsSync(pathG));
    ok(lstatSync(pathG).isDirectory());
    ok(existsSync(new URL('file:///tmp/g/e')));
    deepStrictEqual(readFileSync(new URL('file:///tmp/g/e'), 'utf8'), 'bar');
  },
};

// Copy directory to existing directory
export const copyDirectoryToExistingDirectory = {
  test() {
    setupPaths();
    cpSync(pathB, pathE, { recursive: true });
    ok(existsSync(new URL('file:///tmp/e/e')));
    deepStrictEqual(readFileSync(new URL('file:///tmp/e/e'), 'utf8'), 'bar');
  },
};

// Copy directory to existing file
export const copyDirectoryToExistingFile = {
  test() {
    setupPaths();
    throws(() => cpSync(pathB, pathA, { recursive: true }), {
      code: 'ERR_FS_CP_DIR_TO_NON_DIR',
    });
  },
};

// Copy directory to existing symlink to file
export const copyDirectoryToExistingSymlinkToFile = {
  test() {
    setupPaths();
    throws(() => cpSync(pathB, pathC, { recursive: true, dereference: true }), {
      code: 'ERR_FS_CP_DIR_TO_NON_DIR',
    });
  },
};

// Copy symlink to file
export const copySymlinkToFile = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    // With dereference: false (default), should copy the symlink
    cpSync(pathC, pathG);
    ok(lstatSync(pathG).isSymbolicLink());
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'foo');

    // With dereference: true, should copy the target file
    const pathH = new URL('file:///tmp/h');
    cpSync(pathC, pathH, { dereference: true });
    ok(lstatSync(pathH).isFile());
    deepStrictEqual(readFileSync(pathH, 'utf8'), 'foo');
  },
};

// Copy symlink to directory
export const copySymlinkToDirectory = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    // With dereference: false (default), should copy the symlink
    cpSync(pathD, pathG);
    ok(lstatSync(pathG).isSymbolicLink());
    ok(statSync(pathG).isDirectory());

    // With dereference: true, should copy the target directory
    const pathH = new URL('file:///tmp/h');
    cpSync(pathD, pathH, { recursive: true, dereference: true });
    ok(lstatSync(pathH).isDirectory());
    ok(existsSync(new URL('file:///tmp/h/e')));
  },
};

// Copy symlink to existing file
export const copySymlinkToExistingFile = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    writeFileSync(pathG, 'original');

    // Should replace the file with symlink
    cpSync(pathC, pathG);
    ok(lstatSync(pathG).isSymbolicLink());
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'foo');
  },
};

// Copy symlink to existing directory
export const copySymlinkToExistingDirectory = {
  test() {
    setupPaths();
    throws(() => cpSync(pathC, pathE), {
      code: 'ENOTDIR',
    });
  },
};

// Copy symlink to existing symlink to directory
export const copySymlinkToExistingSymlinkToDirectory = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');
    mkdirSync(pathH);
    symlinkSync(pathH, pathG);

    // Should replace the symlink
    cpSync(pathD, pathG);
    ok(lstatSync(pathG).isSymbolicLink());
    ok(statSync(pathG).isDirectory());
  },
};

// Copy symlink operations with various option combinations
export const copySymlinkWithDereferenceOptions = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');
    const pathI = new URL('file:///tmp/i');

    // Test with force: false, errorOnExist: true
    symlinkSync(pathA, pathG);
    throws(() => cpSync(pathC, pathG, { force: false, errorOnExist: true }), {
      code: 'ERR_FS_CP_EEXIST',
    });

    // Test with force: false, errorOnExist: false
    cpSync(pathC, pathG, { force: false, errorOnExist: false });
    ok(lstatSync(pathG).isSymbolicLink());

    // Test with dereference and errorOnExist combinations
    writeFileSync(pathH, 'existing');
    throws(
      () =>
        cpSync(pathC, pathH, {
          dereference: true,
          force: false,
          errorOnExist: true,
        }),
      {
        code: 'ERR_FS_CP_EEXIST',
      }
    );

    // Test copying to non-existent path
    cpSync(pathC, pathI);
    ok(lstatSync(pathI).isSymbolicLink());
    deepStrictEqual(readFileSync(pathI, 'utf8'), 'foo');
  },
};

// Option validation tests
export const optionValidationTests = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    // Test invalid option types
    throws(() => cpSync(pathA, pathG, { dereference: 'invalid' }), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => cpSync(pathA, pathG, { errorOnExist: 'invalid' }), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => cpSync(pathA, pathG, { force: 'invalid' }), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => cpSync(pathA, pathG, { recursive: 'invalid' }), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => cpSync(pathA, pathG, { preserveTimestamps: 'invalid' }), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => cpSync(pathA, pathG, { verbatimSymlinks: 'invalid' }), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    // Test invalid filter type
    throws(() => cpSync(pathA, pathG, { filter: 'not-a-function' }), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    // Test filter option throws unsupported operation
    throws(() => cpSync(pathA, pathG, { filter: () => true }), {
      code: 'ERR_UNSUPPORTED_OPERATION',
    });

    // Test COPYFILE_FICLONE_FORCE mode
    throws(() => cpSync(pathA, pathG, { mode: 4 }), {
      // COPYFILE_FICLONE_FORCE = 4
      code: 'ERR_INVALID_ARG_VALUE',
    });
  },
};

// Recursive option tests
export const recursiveOptionTests = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');

    // Test copying directory without recursive option should fail
    throws(() => cpSync(pathB, pathG), {
      code: 'ERR_FS_EISDIR',
    });

    // Test with recursive: false explicitly
    throws(() => cpSync(pathB, pathG, { recursive: false }), {
      code: 'ERR_FS_EISDIR',
    });

    // Test copying directory with recursive: true should work
    cpSync(pathB, pathG, { recursive: true });
    ok(existsSync(pathG));
    ok(lstatSync(pathG).isDirectory());
    ok(existsSync(new URL('file:///tmp/g/e')));

    // Test deep directory structure
    mkdirSync(new URL('file:///tmp/deep'));
    mkdirSync(new URL('file:///tmp/deep/nested'));
    mkdirSync(new URL('file:///tmp/deep/nested/very'));
    writeFileSync(new URL('file:///tmp/deep/nested/very/deep.txt'), 'content');

    cpSync(new URL('file:///tmp/deep'), pathH, { recursive: true });
    ok(existsSync(new URL('file:///tmp/h/nested/very/deep.txt')));
    deepStrictEqual(
      readFileSync(new URL('file:///tmp/h/nested/very/deep.txt'), 'utf8'),
      'content'
    );
  },
};

// PreserveTimestamps option tests
export const preserveTimestampsTests = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');

    // Get original timestamps
    const originalStat = lstatSync(pathA);
    const originalMtime = originalStat.mtime;

    // Copy without preserveTimestamps (default behavior)
    cpSync(pathA, pathG);
    const copiedStat = lstatSync(pathG);

    // Copy with preserveTimestamps: true
    cpSync(pathA, pathH, { preserveTimestamps: true });
    const preservedStat = lstatSync(pathH);

    // The preserved timestamps should match the original
    // Note: This test depends on the implementation actually preserving timestamps
    // Since the implementation is noted as having this option, we test it
    ok(
      Math.abs(preservedStat.mtime.getTime() - originalMtime.getTime()) < 1000
    );
  },
};

// Error handling edge cases
export const errorHandlingTests = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const nonExistent = new URL('file:///tmp/nonexistent');

    // Test copying from non-existent source
    throws(() => cpSync(nonExistent, pathG), {
      code: 'ENOENT',
    });
  },
};

// Symlink edge cases
export const symlinkEdgeCases = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');
    const pathI = new URL('file:///tmp/i');
    const pathJ = new URL('file:///tmp/j');

    // Create a broken symlink
    const brokenTarget = new URL('file:///tmp/broken-target');
    symlinkSync(brokenTarget, pathG);

    // Test copying broken symlink without dereference
    cpSync(pathG, pathH);
    ok(lstatSync(pathH).isSymbolicLink());

    // Test copying broken symlink with dereference should fail
    throws(() => cpSync(pathG, pathI, { dereference: true }), {
      code: 'ENOENT',
    });

    // Create symlink chain: J -> I -> A
    symlinkSync(pathA, pathI);
    symlinkSync(pathI, pathJ);

    // Test copying symlink chain
    const pathK = new URL('file:///tmp/k');
    cpSync(pathJ, pathK);
    ok(lstatSync(pathK).isSymbolicLink());
    deepStrictEqual(readFileSync(pathK, 'utf8'), 'foo');
  },
};

// Combined options tests
export const combinedOptionsTests = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    // Test multiple options together
    cpSync(pathB, pathG, {
      recursive: true,
      dereference: true,
      force: true,
      preserveTimestamps: true,
      errorOnExist: false,
    });

    ok(existsSync(pathG));
    ok(lstatSync(pathG).isDirectory());
  },
};

// Path type tests
export const pathTypeTests = {
  test() {
    setupPaths();

    // Test with Buffer paths
    const pathG = new URL('file:///tmp/g');
    const bufferSrc = Buffer.from('/tmp/a');
    const bufferDest = Buffer.from('/tmp/g');

    // Note: The implementation uses normalizePath which should handle Buffer inputs
    // This tests the path normalization behavior
    cpSync(bufferSrc, bufferDest);
    ok(existsSync(pathG));
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'foo');
  },
};

// ===== CALLBACK-BASED API TESTS =====

// Simple file copy - callback version
export const simpleFileCopyCallback = {
  async test() {
    setupPaths();
    ok(existsSync(pathA));
    deepStrictEqual(readFileSync(pathA, 'utf8'), 'foo');
    ok(!existsSync(pathF));

    await new Promise((resolve, reject) => {
      cp(pathA, pathF, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(existsSync(pathF));
    deepStrictEqual(readFileSync(pathF, 'utf8'), 'foo');
    writeFileSync(pathA, 'baz');
    deepStrictEqual(readFileSync(pathA, 'utf8'), 'baz');
    deepStrictEqual(readFileSync(pathF, 'utf8'), 'foo');

    // Test with errorOnExist: true
    await new Promise((resolve, reject) => {
      cp(pathA, pathF, { errorOnExist: true }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    // Test error case: force: false, errorOnExist: true
    await new Promise((resolve) => {
      cp(pathA, pathF, { force: false, errorOnExist: true }, (err) => {
        ok(err);
        strictEqual(err.code, 'ERR_FS_CP_EEXIST');
        resolve();
      });
    });

    // Test error case: copying file to directory
    await new Promise((resolve) => {
      cp(pathA, pathB, (err) => {
        ok(err);
        strictEqual(err.code, 'ERR_FS_CP_NON_DIR_TO_DIR');
        resolve();
      });
    });

    // Test copying to symlink to directory
    await new Promise((resolve) => {
      cp(pathA, pathD, { dereference: true }, (err) => {
        ok(err);
        strictEqual(err.code, 'ERR_FS_CP_NON_DIR_TO_DIR');
        resolve();
      });
    });

    // Test copying to symlink without dereference
    await new Promise((resolve, reject) => {
      cp(pathA, pathD, (err) => {
        if (err) reject(err);
        else {
          ok(lstatSync(pathD).isFile());
          resolve();
        }
      });
    });
  },
};

// Copy file to non-existent directory - callback version
export const copyFileToNonExistentDirectoryCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/nonexistent');
    const pathH = new URL('file:///tmp/nonexistent/g');
    ok(!existsSync(pathG));
    ok(!existsSync(pathH));

    await new Promise((resolve, reject) => {
      cp(pathA, pathH, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(existsSync(pathG));
    ok(existsSync(pathH));
  },
};

// Copy file to existing file with force - callback version
export const copyFileToExistingFileWithForceCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    writeFileSync(pathG, 'original');

    await new Promise((resolve, reject) => {
      cp(pathA, pathG, { force: true }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    deepStrictEqual(readFileSync(pathG, 'utf8'), 'foo');
  },
};

// Copy file to existing file without force - callback version
export const copyFileToExistingFileWithoutForceCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    writeFileSync(pathG, 'original');

    await new Promise((resolve, reject) => {
      cp(pathA, pathG, { force: false }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    deepStrictEqual(readFileSync(pathG, 'utf8'), 'original');
  },
};

// Copy file to existing directory - callback version
export const copyFileToExistingDirectoryCallback = {
  async test() {
    setupPaths();

    await new Promise((resolve) => {
      cp(pathA, pathB, (err) => {
        ok(err);
        strictEqual(err.code, 'ERR_FS_CP_NON_DIR_TO_DIR');
        resolve();
      });
    });
  },
};

// Copy file to existing symlink to directory - callback version
export const copyFileToExistingSymlinkToDirectoryCallback = {
  async test() {
    setupPaths();

    // With dereference: true, should throw EISDIR
    await new Promise((resolve) => {
      cp(pathA, pathD, { dereference: true }, (err) => {
        ok(err);
        strictEqual(err.code, 'ERR_FS_CP_NON_DIR_TO_DIR');
        resolve();
      });
    });

    // Without dereference (default), should replace the symlink
    await new Promise((resolve, reject) => {
      cp(pathA, pathD, (err) => {
        if (err) reject(err);
        else {
          ok(lstatSync(pathD).isFile());
          deepStrictEqual(readFileSync(pathD, 'utf8'), 'foo');
          resolve();
        }
      });
    });
  },
};

// Copy directory to non-existent directory - callback version
export const copyDirectoryToNonExistentDirectoryCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    await new Promise((resolve, reject) => {
      cp(pathB, pathG, { recursive: true }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(existsSync(pathG));
    ok(lstatSync(pathG).isDirectory());
    ok(existsSync(new URL('file:///tmp/g/e')));
    deepStrictEqual(readFileSync(new URL('file:///tmp/g/e'), 'utf8'), 'bar');
  },
};

// Copy directory to existing directory - callback version
export const copyDirectoryToExistingDirectoryCallback = {
  async test() {
    setupPaths();

    await new Promise((resolve, reject) => {
      cp(pathB, pathE, { recursive: true }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(existsSync(new URL('file:///tmp/e/e')));
    deepStrictEqual(readFileSync(new URL('file:///tmp/e/e'), 'utf8'), 'bar');
  },
};

// Copy directory to existing file - callback version
export const copyDirectoryToExistingFileCallback = {
  async test() {
    setupPaths();

    await new Promise((resolve) => {
      cp(pathB, pathA, { recursive: true }, (err) => {
        ok(err);
        strictEqual(err.code, 'ERR_FS_CP_DIR_TO_NON_DIR');
        resolve();
      });
    });
  },
};

// Copy directory to existing symlink to file - callback version
export const copyDirectoryToExistingSymlinkToFileCallback = {
  async test() {
    setupPaths();

    await new Promise((resolve) => {
      cp(pathB, pathC, { recursive: true, dereference: true }, (err) => {
        ok(err);
        strictEqual(err.code, 'ERR_FS_CP_DIR_TO_NON_DIR');
        resolve();
      });
    });
  },
};

// Copy symlink to file - callback version
export const copySymlinkToFileCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    // With dereference: false (default), should copy the symlink
    await new Promise((resolve, reject) => {
      cp(pathC, pathG, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(lstatSync(pathG).isSymbolicLink());
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'foo');

    // With dereference: true, should copy the target file
    const pathH = new URL('file:///tmp/h');
    await new Promise((resolve, reject) => {
      cp(pathC, pathH, { dereference: true }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(lstatSync(pathH).isFile());
    deepStrictEqual(readFileSync(pathH, 'utf8'), 'foo');
  },
};

// Copy symlink to directory - callback version
export const copySymlinkToDirectoryCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    // With dereference: false (default), should copy the symlink
    await new Promise((resolve, reject) => {
      cp(pathD, pathG, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(lstatSync(pathG).isSymbolicLink());
    ok(statSync(pathG).isDirectory());

    // With dereference: true, should copy the target directory
    const pathH = new URL('file:///tmp/h');
    await new Promise((resolve, reject) => {
      cp(pathD, pathH, { recursive: true, dereference: true }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(lstatSync(pathH).isDirectory());
    ok(existsSync(new URL('file:///tmp/h/e')));
  },
};

// Copy symlink to existing file - callback version
export const copySymlinkToExistingFileCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    writeFileSync(pathG, 'original');

    // Should replace the file with symlink
    await new Promise((resolve, reject) => {
      cp(pathC, pathG, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(lstatSync(pathG).isSymbolicLink());
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'foo');
  },
};

// Copy symlink to existing directory - callback version
export const copySymlinkToExistingDirectoryCallback = {
  async test() {
    setupPaths();

    await new Promise((resolve) => {
      cp(pathC, pathE, (err) => {
        ok(err);
        strictEqual(err.code, 'ENOTDIR');
        resolve();
      });
    });
  },
};

// Copy symlink to existing symlink to directory - callback version
export const copySymlinkToExistingSymlinkToDirectoryCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');
    mkdirSync(pathH);
    symlinkSync(pathH, pathG);

    // Should replace the symlink
    await new Promise((resolve, reject) => {
      cp(pathD, pathG, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(lstatSync(pathG).isSymbolicLink());
    ok(statSync(pathG).isDirectory());
  },
};

// Copy symlink operations with various option combinations - callback version
export const copySymlinkWithDereferenceOptionsCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');
    const pathI = new URL('file:///tmp/i');

    // Test with force: false, errorOnExist: true
    symlinkSync(pathA, pathG);
    await new Promise((resolve) => {
      cp(pathC, pathG, { force: false, errorOnExist: true }, (err) => {
        ok(err);
        strictEqual(err.code, 'ERR_FS_CP_EEXIST');
        resolve();
      });
    });

    // Test with force: false, errorOnExist: false
    await new Promise((resolve, reject) => {
      cp(pathC, pathG, { force: false, errorOnExist: false }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
    ok(lstatSync(pathG).isSymbolicLink());

    // Test with dereference and errorOnExist combinations
    writeFileSync(pathH, 'existing');
    await new Promise((resolve) => {
      cp(
        pathC,
        pathH,
        { dereference: true, force: false, errorOnExist: true },
        (err) => {
          ok(err);
          strictEqual(err.code, 'ERR_FS_CP_EEXIST');
          resolve();
        }
      );
    });

    // Test copying to non-existent path
    await new Promise((resolve, reject) => {
      cp(pathC, pathI, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
    ok(lstatSync(pathI).isSymbolicLink());
    deepStrictEqual(readFileSync(pathI, 'utf8'), 'foo');
  },
};

// Option validation tests - callback version
export const optionValidationTestsCallback = {
  test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    // Test invalid option types - these throw synchronously
    throws(() => cp(pathA, pathG, { dereference: 'invalid' }, () => {}), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => cp(pathA, pathG, { errorOnExist: 'invalid' }, () => {}), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => cp(pathA, pathG, { force: 'invalid' }, () => {}), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => cp(pathA, pathG, { recursive: 'invalid' }, () => {}), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(
      () => cp(pathA, pathG, { preserveTimestamps: 'invalid' }, () => {}),
      {
        code: 'ERR_INVALID_ARG_TYPE',
      }
    );

    throws(() => cp(pathA, pathG, { verbatimSymlinks: 'invalid' }, () => {}), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    // Test invalid filter type - throws synchronously
    throws(() => cp(pathA, pathG, { filter: 'not-a-function' }, () => {}), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    // Test filter option throws unsupported operation - throws synchronously
    throws(() => cp(pathA, pathG, { filter: () => true }, () => {}), {
      code: 'ERR_UNSUPPORTED_OPERATION',
    });

    // Test COPYFILE_FICLONE_FORCE mode - throws synchronously
    throws(() => cp(pathA, pathG, { mode: 4 }, () => {}), {
      code: 'ERR_INVALID_ARG_VALUE',
    });
  },
};

// Recursive option tests - callback version
export const recursiveOptionTestsCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');

    // Test copying directory without recursive option should fail
    await new Promise((resolve) => {
      cp(pathB, pathG, (err) => {
        ok(err);
        strictEqual(err.code, 'ERR_FS_EISDIR');
        resolve();
      });
    });

    // Test with recursive: false explicitly
    await new Promise((resolve) => {
      cp(pathB, pathG, { recursive: false }, (err) => {
        ok(err);
        strictEqual(err.code, 'ERR_FS_EISDIR');
        resolve();
      });
    });

    // Test copying directory with recursive: true should work
    await new Promise((resolve, reject) => {
      cp(pathB, pathG, { recursive: true }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(existsSync(pathG));
    ok(lstatSync(pathG).isDirectory());
    ok(existsSync(new URL('file:///tmp/g/e')));

    // Test deep directory structure
    mkdirSync(new URL('file:///tmp/deep'));
    mkdirSync(new URL('file:///tmp/deep/nested'));
    mkdirSync(new URL('file:///tmp/deep/nested/very'));
    writeFileSync(new URL('file:///tmp/deep/nested/very/deep.txt'), 'content');

    await new Promise((resolve, reject) => {
      cp(new URL('file:///tmp/deep'), pathH, { recursive: true }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(existsSync(new URL('file:///tmp/h/nested/very/deep.txt')));
    deepStrictEqual(
      readFileSync(new URL('file:///tmp/h/nested/very/deep.txt'), 'utf8'),
      'content'
    );
  },
};

// PreserveTimestamps option tests - callback version
export const preserveTimestampsTestsCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');

    // Get original timestamps
    const originalStat = lstatSync(pathA);
    const originalMtime = originalStat.mtime;

    // Copy without preserveTimestamps (default behavior)
    await new Promise((resolve, reject) => {
      cp(pathA, pathG, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
    const copiedStat = lstatSync(pathG);

    // Copy with preserveTimestamps: true
    await new Promise((resolve, reject) => {
      cp(pathA, pathH, { preserveTimestamps: true }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
    const preservedStat = lstatSync(pathH);

    ok(
      Math.abs(preservedStat.mtime.getTime() - originalMtime.getTime()) < 1000
    );
  },
};

// Error handling edge cases - callback version
export const errorHandlingTestsCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const nonExistent = new URL('file:///tmp/nonexistent');

    // Test copying from non-existent source
    await new Promise((resolve) => {
      cp(nonExistent, pathG, (err) => {
        ok(err);
        strictEqual(err.code, 'ENOENT');
        resolve();
      });
    });
  },
};

// Symlink edge cases - callback version
export const symlinkEdgeCasesCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');
    const pathI = new URL('file:///tmp/i');
    const pathJ = new URL('file:///tmp/j');

    // Create a broken symlink
    const brokenTarget = new URL('file:///tmp/broken-target');
    symlinkSync(brokenTarget, pathG);

    // Test copying broken symlink without dereference
    await new Promise((resolve, reject) => {
      cp(pathG, pathH, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
    ok(lstatSync(pathH).isSymbolicLink());

    // Test copying broken symlink with dereference should fail
    await new Promise((resolve) => {
      cp(pathG, pathI, { dereference: true }, (err) => {
        ok(err);
        strictEqual(err.code, 'ENOENT');
        resolve();
      });
    });

    // Create symlink chain: J -> I -> A
    symlinkSync(pathA, pathI);
    symlinkSync(pathI, pathJ);

    // Test copying symlink chain
    const pathK = new URL('file:///tmp/k');
    await new Promise((resolve, reject) => {
      cp(pathJ, pathK, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(lstatSync(pathK).isSymbolicLink());
    deepStrictEqual(readFileSync(pathK, 'utf8'), 'foo');
  },
};

// Combined options tests - callback version
export const combinedOptionsTestsCallback = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    // Test multiple options together
    await new Promise((resolve, reject) => {
      cp(
        pathB,
        pathG,
        {
          recursive: true,
          dereference: true,
          force: true,
          preserveTimestamps: true,
          errorOnExist: false,
        },
        (err) => {
          if (err) reject(err);
          else resolve();
        }
      );
    });

    ok(existsSync(pathG));
    ok(lstatSync(pathG).isDirectory());
  },
};

// Path type tests - callback version
export const pathTypeTestsCallback = {
  async test() {
    setupPaths();

    // Test with Buffer paths
    const pathG = new URL('file:///tmp/g');
    const bufferSrc = Buffer.from('/tmp/a');
    const bufferDest = Buffer.from('/tmp/g');

    await new Promise((resolve, reject) => {
      cp(bufferSrc, bufferDest, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    ok(existsSync(pathG));
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'foo');
  },
};

// ===== PROMISES-BASED API TESTS =====

// Simple file copy - promises version
export const simpleFileCopyPromises = {
  async test() {
    setupPaths();
    ok(existsSync(pathA));
    deepStrictEqual(readFileSync(pathA, 'utf8'), 'foo');
    ok(!existsSync(pathF));

    await fsPromises.cp(pathA, pathF);
    ok(existsSync(pathF));
    deepStrictEqual(readFileSync(pathF, 'utf8'), 'foo');
    writeFileSync(pathA, 'baz');
    deepStrictEqual(readFileSync(pathA, 'utf8'), 'baz');
    deepStrictEqual(readFileSync(pathF, 'utf8'), 'foo');

    // Test with errorOnExist: true
    await fsPromises.cp(pathA, pathF, { errorOnExist: true });

    // Test error case: force: false, errorOnExist: true
    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathF, { force: false, errorOnExist: true });
      },
      { code: 'ERR_FS_CP_EEXIST' }
    );

    // Test error case: copying file to directory
    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathB);
      },
      { code: 'ERR_FS_CP_NON_DIR_TO_DIR' }
    );

    // Test copying to symlink to directory
    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathD, { dereference: true });
      },
      { code: 'ERR_FS_CP_NON_DIR_TO_DIR' }
    );

    // Test copying to symlink without dereference
    await fsPromises.cp(pathA, pathD);
    ok(lstatSync(pathD).isFile());
  },
};

// Copy file to non-existent directory - promises version
export const copyFileToNonExistentDirectoryPromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/nonexistent');
    const pathH = new URL('file:///tmp/nonexistent/g');
    ok(!existsSync(pathG));
    ok(!existsSync(pathH));

    await fsPromises.cp(pathA, pathH);
    ok(existsSync(pathG));
    ok(existsSync(pathH));
  },
};

// Copy file to existing file with force - promises version
export const copyFileToExistingFileWithForcePromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    writeFileSync(pathG, 'original');

    await fsPromises.cp(pathA, pathG, { force: true });
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'foo');
  },
};

// Copy file to existing file without force - promises version
export const copyFileToExistingFileWithoutForcePromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    writeFileSync(pathG, 'original');

    await fsPromises.cp(pathA, pathG, { force: false });
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'original');
  },
};

// Copy file to existing directory - promises version
export const copyFileToExistingDirectoryPromises = {
  async test() {
    setupPaths();

    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathB);
      },
      { code: 'ERR_FS_CP_NON_DIR_TO_DIR' }
    );
  },
};

// Copy file to existing symlink to directory - promises version
export const copyFileToExistingSymlinkToDirectoryPromises = {
  async test() {
    setupPaths();

    // With dereference: true, should throw EISDIR
    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathD, { dereference: true });
      },
      { code: 'ERR_FS_CP_NON_DIR_TO_DIR' }
    );

    // Without dereference (default), should replace the symlink
    await fsPromises.cp(pathA, pathD);
    ok(lstatSync(pathD).isFile());
    deepStrictEqual(readFileSync(pathD, 'utf8'), 'foo');
  },
};

// Copy directory to non-existent directory - promises version
export const copyDirectoryToNonExistentDirectoryPromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    await fsPromises.cp(pathB, pathG, { recursive: true });
    ok(existsSync(pathG));
    ok(lstatSync(pathG).isDirectory());
    ok(existsSync(new URL('file:///tmp/g/e')));
    deepStrictEqual(readFileSync(new URL('file:///tmp/g/e'), 'utf8'), 'bar');
  },
};

// Copy directory to existing directory - promises version
export const copyDirectoryToExistingDirectoryPromises = {
  async test() {
    setupPaths();

    await fsPromises.cp(pathB, pathE, { recursive: true });
    ok(existsSync(new URL('file:///tmp/e/e')));
    deepStrictEqual(readFileSync(new URL('file:///tmp/e/e'), 'utf8'), 'bar');
  },
};

// Copy directory to existing file - promises version
export const copyDirectoryToExistingFilePromises = {
  async test() {
    setupPaths();

    await rejects(
      async () => {
        await fsPromises.cp(pathB, pathA, { recursive: true });
      },
      { code: 'ERR_FS_CP_DIR_TO_NON_DIR' }
    );
  },
};

// Copy directory to existing symlink to file - promises version
export const copyDirectoryToExistingSymlinkToFilePromises = {
  async test() {
    setupPaths();

    await rejects(
      async () => {
        await fsPromises.cp(pathB, pathC, {
          recursive: true,
          dereference: true,
        });
      },
      { code: 'ERR_FS_CP_DIR_TO_NON_DIR' }
    );
  },
};

// Copy symlink to file - promises version
export const copySymlinkToFilePromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    // With dereference: false (default), should copy the symlink
    await fsPromises.cp(pathC, pathG);
    ok(lstatSync(pathG).isSymbolicLink());
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'foo');

    // With dereference: true, should copy the target file
    const pathH = new URL('file:///tmp/h');
    await fsPromises.cp(pathC, pathH, { dereference: true });
    ok(lstatSync(pathH).isFile());
    deepStrictEqual(readFileSync(pathH, 'utf8'), 'foo');
  },
};

// Copy symlink to directory - promises version
export const copySymlinkToDirectoryPromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    // With dereference: false (default), should copy the symlink
    await fsPromises.cp(pathD, pathG);
    ok(lstatSync(pathG).isSymbolicLink());
    ok(statSync(pathG).isDirectory());

    // With dereference: true, should copy the target directory
    const pathH = new URL('file:///tmp/h');
    await fsPromises.cp(pathD, pathH, { recursive: true, dereference: true });
    ok(lstatSync(pathH).isDirectory());
    ok(existsSync(new URL('file:///tmp/h/e')));
  },
};

// Copy symlink to existing file - promises version
export const copySymlinkToExistingFilePromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    writeFileSync(pathG, 'original');

    // Should replace the file with symlink
    await fsPromises.cp(pathC, pathG);
    ok(lstatSync(pathG).isSymbolicLink());
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'foo');
  },
};

// Copy symlink to existing directory - promises version
export const copySymlinkToExistingDirectoryPromises = {
  async test() {
    setupPaths();

    await rejects(
      async () => {
        await fsPromises.cp(pathC, pathE);
      },
      { code: 'ENOTDIR' }
    );
  },
};

// Copy symlink to existing symlink to directory - promises version
export const copySymlinkToExistingSymlinkToDirectoryPromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');
    mkdirSync(pathH);
    symlinkSync(pathH, pathG);

    // Should replace the symlink
    await fsPromises.cp(pathD, pathG);
    ok(lstatSync(pathG).isSymbolicLink());
    ok(statSync(pathG).isDirectory());
  },
};

// Copy symlink operations with various option combinations - promises version
export const copySymlinkWithDereferenceOptionsPromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');
    const pathI = new URL('file:///tmp/i');

    // Test with force: false, errorOnExist: true
    symlinkSync(pathA, pathG);
    await rejects(
      async () => {
        await fsPromises.cp(pathC, pathG, { force: false, errorOnExist: true });
      },
      { code: 'ERR_FS_CP_EEXIST' }
    );

    // Test with force: false, errorOnExist: false
    await fsPromises.cp(pathC, pathG, { force: false, errorOnExist: false });
    ok(lstatSync(pathG).isSymbolicLink());

    // Test with dereference and errorOnExist combinations
    writeFileSync(pathH, 'existing');
    await rejects(
      async () => {
        await fsPromises.cp(pathC, pathH, {
          dereference: true,
          force: false,
          errorOnExist: true,
        });
      },
      { code: 'ERR_FS_CP_EEXIST' }
    );

    // Test copying to non-existent path
    await fsPromises.cp(pathC, pathI);
    ok(lstatSync(pathI).isSymbolicLink());
    deepStrictEqual(readFileSync(pathI, 'utf8'), 'foo');
  },
};

// Option validation tests - promises version
export const optionValidationTestsPromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    // Test invalid option types
    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathG, { dereference: 'invalid' });
      },
      { code: 'ERR_INVALID_ARG_TYPE' }
    );

    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathG, { errorOnExist: 'invalid' });
      },
      { code: 'ERR_INVALID_ARG_TYPE' }
    );

    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathG, { force: 'invalid' });
      },
      { code: 'ERR_INVALID_ARG_TYPE' }
    );

    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathG, { recursive: 'invalid' });
      },
      { code: 'ERR_INVALID_ARG_TYPE' }
    );

    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathG, { preserveTimestamps: 'invalid' });
      },
      { code: 'ERR_INVALID_ARG_TYPE' }
    );

    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathG, { verbatimSymlinks: 'invalid' });
      },
      { code: 'ERR_INVALID_ARG_TYPE' }
    );

    // Test invalid filter type
    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathG, { filter: 'not-a-function' });
      },
      { code: 'ERR_INVALID_ARG_TYPE' }
    );

    // Test filter option throws unsupported operation
    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathG, { filter: () => true });
      },
      { code: 'ERR_UNSUPPORTED_OPERATION' }
    );

    // Test COPYFILE_FICLONE_FORCE mode
    await rejects(
      async () => {
        await fsPromises.cp(pathA, pathG, { mode: 4 });
      },
      { code: 'ERR_INVALID_ARG_VALUE' }
    );
  },
};

// Recursive option tests - promises version
export const recursiveOptionTestsPromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');

    // Test copying directory without recursive option should fail
    await rejects(
      async () => {
        await fsPromises.cp(pathB, pathG);
      },
      { code: 'ERR_FS_EISDIR' }
    );

    // Test with recursive: false explicitly
    await rejects(
      async () => {
        await fsPromises.cp(pathB, pathG, { recursive: false });
      },
      { code: 'ERR_FS_EISDIR' }
    );

    // Test copying directory with recursive: true should work
    await fsPromises.cp(pathB, pathG, { recursive: true });
    ok(existsSync(pathG));
    ok(lstatSync(pathG).isDirectory());
    ok(existsSync(new URL('file:///tmp/g/e')));

    // Test deep directory structure
    mkdirSync(new URL('file:///tmp/deep'));
    mkdirSync(new URL('file:///tmp/deep/nested'));
    mkdirSync(new URL('file:///tmp/deep/nested/very'));
    writeFileSync(new URL('file:///tmp/deep/nested/very/deep.txt'), 'content');

    await fsPromises.cp(new URL('file:///tmp/deep'), pathH, {
      recursive: true,
    });
    ok(existsSync(new URL('file:///tmp/h/nested/very/deep.txt')));
    deepStrictEqual(
      readFileSync(new URL('file:///tmp/h/nested/very/deep.txt'), 'utf8'),
      'content'
    );
  },
};

// PreserveTimestamps option tests - promises version
export const preserveTimestampsTestsPromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');

    // Get original timestamps
    const originalStat = lstatSync(pathA);
    const originalMtime = originalStat.mtime;

    // Copy without preserveTimestamps (default behavior)
    await fsPromises.cp(pathA, pathG);
    const copiedStat = lstatSync(pathG);

    // Copy with preserveTimestamps: true
    await fsPromises.cp(pathA, pathH, { preserveTimestamps: true });
    const preservedStat = lstatSync(pathH);

    ok(
      Math.abs(preservedStat.mtime.getTime() - originalMtime.getTime()) < 1000
    );
  },
};

// Error handling edge cases - promises version
export const errorHandlingTestsPromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const nonExistent = new URL('file:///tmp/nonexistent');

    // Test copying from non-existent source
    await rejects(
      async () => {
        await fsPromises.cp(nonExistent, pathG);
      },
      { code: 'ENOENT' }
    );
  },
};

// Symlink edge cases - promises version
export const symlinkEdgeCasesPromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');
    const pathH = new URL('file:///tmp/h');
    const pathI = new URL('file:///tmp/i');
    const pathJ = new URL('file:///tmp/j');

    // Create a broken symlink
    const brokenTarget = new URL('file:///tmp/broken-target');
    symlinkSync(brokenTarget, pathG);

    // Test copying broken symlink without dereference
    await fsPromises.cp(pathG, pathH);
    ok(lstatSync(pathH).isSymbolicLink());

    // Test copying broken symlink with dereference should fail
    await rejects(
      async () => {
        await fsPromises.cp(pathG, pathI, { dereference: true });
      },
      { code: 'ENOENT' }
    );

    // Create symlink chain: J -> I -> A
    symlinkSync(pathA, pathI);
    symlinkSync(pathI, pathJ);

    // Test copying symlink chain
    const pathK = new URL('file:///tmp/k');
    await fsPromises.cp(pathJ, pathK);

    ok(lstatSync(pathK).isSymbolicLink());
    deepStrictEqual(readFileSync(pathK, 'utf8'), 'foo');
  },
};

// Combined options tests - promises version
export const combinedOptionsTestsPromises = {
  async test() {
    setupPaths();
    const pathG = new URL('file:///tmp/g');

    // Test multiple options together
    await fsPromises.cp(pathB, pathG, {
      recursive: true,
      dereference: true,
      force: true,
      preserveTimestamps: true,
      errorOnExist: false,
    });

    ok(existsSync(pathG));
    ok(lstatSync(pathG).isDirectory());
  },
};

// Path type tests - promises version
export const pathTypeTestsPromises = {
  async test() {
    setupPaths();

    // Test with Buffer paths
    const pathG = new URL('file:///tmp/g');
    const bufferSrc = Buffer.from('/tmp/a');
    const bufferDest = Buffer.from('/tmp/g');

    await fsPromises.cp(bufferSrc, bufferDest);
    ok(existsSync(pathG));
    deepStrictEqual(readFileSync(pathG, 'utf8'), 'foo');
  },
};

// Deep directory structure tests - sync version
export const deepDirectoryStructureTestsSync = {
  test() {
    const sourceRoot = '/tmp/deep_source';
    const destRoot = '/tmp/deep_dest';

    // Create source directory structure
    mkdirSync(sourceRoot, { recursive: true });
    mkdirSync(`${sourceRoot}/level1`, { recursive: true });
    mkdirSync(`${sourceRoot}/level1/level2`, { recursive: true });
    mkdirSync(`${sourceRoot}/level1/level2/level3`, { recursive: true });
    mkdirSync(`${sourceRoot}/level1/level2/level3/level4`, { recursive: true });
    mkdirSync(`${sourceRoot}/level1/level2/level3/level4/level5`, {
      recursive: true,
    });

    // Create files in various levels
    writeFileSync(`${sourceRoot}/root_file.txt`, 'root content');
    writeFileSync(`${sourceRoot}/level1/level1_file.txt`, 'level1 content');
    writeFileSync(
      `${sourceRoot}/level1/level2/level2_file.txt`,
      'level2 content'
    );
    writeFileSync(
      `${sourceRoot}/level1/level2/level3/level3_file.txt`,
      'level3 content'
    );
    writeFileSync(
      `${sourceRoot}/level1/level2/level3/level4/level4_file.txt`,
      'level4 content'
    );
    writeFileSync(
      `${sourceRoot}/level1/level2/level3/level4/level5/level5_file.txt`,
      'level5 content'
    );

    // Create symlinks (will not be followed)
    writeFileSync(`${sourceRoot}/link_target.txt`, 'link target content');
    symlinkSync(
      `${sourceRoot}/link_target.txt`,
      `${sourceRoot}/level1/level2/symlink_to_file`
    );
    symlinkSync(
      `${sourceRoot}/level1`,
      `${sourceRoot}/level1/level2/level3/symlink_to_dir`
    );

    // Create destination directory structure with some existing files (duplicates)
    mkdirSync(destRoot, { recursive: true });
    mkdirSync(`${destRoot}/level1`, { recursive: true });
    mkdirSync(`${destRoot}/level1/level2`, { recursive: true });
    mkdirSync(`${destRoot}/existing_dir`, { recursive: true });

    // Create files that will be duplicates (should be ignored)
    writeFileSync(`${destRoot}/duplicate_file.txt`, 'existing content');
    writeFileSync(
      `${destRoot}/level1/duplicate_file.txt`,
      'existing level1 content'
    );

    // Add some files to source that match existing files (duplicates)
    writeFileSync(
      `${sourceRoot}/duplicate_file.txt`,
      'source duplicate content'
    );
    writeFileSync(
      `${sourceRoot}/level1/duplicate_file.txt`,
      'source level1 duplicate content'
    );

    // Perform recursive copy (should ignore duplicates and not follow symlinks)
    cpSync(sourceRoot, destRoot, { recursive: true, force: false });

    // Verify destination structure
    ok(existsSync(`${destRoot}/root_file.txt`));
    ok(existsSync(`${destRoot}/level1/level1_file.txt`));
    ok(existsSync(`${destRoot}/level1/level2/level2_file.txt`));
    ok(existsSync(`${destRoot}/level1/level2/level3/level3_file.txt`));
    ok(existsSync(`${destRoot}/level1/level2/level3/level4/level4_file.txt`));
    ok(
      existsSync(
        `${destRoot}/level1/level2/level3/level4/level5/level5_file.txt`
      )
    );

    // Verify file contents
    deepStrictEqual(
      readFileSync(`${destRoot}/root_file.txt`, 'utf8'),
      'root content'
    );
    deepStrictEqual(
      readFileSync(`${destRoot}/level1/level1_file.txt`, 'utf8'),
      'level1 content'
    );
    deepStrictEqual(
      readFileSync(`${destRoot}/level1/level2/level2_file.txt`, 'utf8'),
      'level2 content'
    );
    deepStrictEqual(
      readFileSync(`${destRoot}/level1/level2/level3/level3_file.txt`, 'utf8'),
      'level3 content'
    );
    deepStrictEqual(
      readFileSync(
        `${destRoot}/level1/level2/level3/level4/level4_file.txt`,
        'utf8'
      ),
      'level4 content'
    );
    deepStrictEqual(
      readFileSync(
        `${destRoot}/level1/level2/level3/level4/level5/level5_file.txt`,
        'utf8'
      ),
      'level5 content'
    );

    // Verify symlinks are copied as symlinks (not followed)
    ok(lstatSync(`${destRoot}/level1/level2/symlink_to_file`).isSymbolicLink());
    ok(
      lstatSync(
        `${destRoot}/level1/level2/level3/symlink_to_dir`
      ).isSymbolicLink()
    );

    // Verify duplicate files were ignored (original content preserved)
    deepStrictEqual(
      readFileSync(`${destRoot}/duplicate_file.txt`, 'utf8'),
      'existing content'
    );
    deepStrictEqual(
      readFileSync(`${destRoot}/level1/duplicate_file.txt`, 'utf8'),
      'existing level1 content'
    );

    // Verify existing directory structure remains
    ok(existsSync(`${destRoot}/existing_dir`));
  },
};

// Deep directory structure tests - callback version
export const deepDirectoryStructureTestsCallback = {
  async test() {
    const sourceRoot = '/tmp/deep_source_cb';
    const destRoot = '/tmp/deep_dest_cb';

    // Create source directory structure
    mkdirSync(sourceRoot, { recursive: true });
    mkdirSync(`${sourceRoot}/level1`, { recursive: true });
    mkdirSync(`${sourceRoot}/level1/level2`, { recursive: true });
    mkdirSync(`${sourceRoot}/level1/level2/level3`, { recursive: true });
    mkdirSync(`${sourceRoot}/level1/level2/level3/level4`, { recursive: true });
    mkdirSync(`${sourceRoot}/level1/level2/level3/level4/level5`, {
      recursive: true,
    });

    // Create files in various levels
    writeFileSync(`${sourceRoot}/root_file.txt`, 'root content');
    writeFileSync(`${sourceRoot}/level1/level1_file.txt`, 'level1 content');
    writeFileSync(
      `${sourceRoot}/level1/level2/level2_file.txt`,
      'level2 content'
    );
    writeFileSync(
      `${sourceRoot}/level1/level2/level3/level3_file.txt`,
      'level3 content'
    );
    writeFileSync(
      `${sourceRoot}/level1/level2/level3/level4/level4_file.txt`,
      'level4 content'
    );
    writeFileSync(
      `${sourceRoot}/level1/level2/level3/level4/level5/level5_file.txt`,
      'level5 content'
    );

    // Create symlinks (will not be followed)
    writeFileSync(`${sourceRoot}/link_target.txt`, 'link target content');
    symlinkSync(
      `${sourceRoot}/link_target.txt`,
      `${sourceRoot}/level1/level2/symlink_to_file`
    );
    symlinkSync(
      `${sourceRoot}/level1`,
      `${sourceRoot}/level1/level2/level3/symlink_to_dir`
    );

    // Create destination directory structure with some existing files (duplicates)
    mkdirSync(destRoot, { recursive: true });
    mkdirSync(`${destRoot}/level1`, { recursive: true });
    mkdirSync(`${destRoot}/level1/level2`, { recursive: true });
    mkdirSync(`${destRoot}/existing_dir`, { recursive: true });

    // Create files that will be duplicates (should be ignored)
    writeFileSync(`${destRoot}/duplicate_file.txt`, 'existing content');
    writeFileSync(
      `${destRoot}/level1/duplicate_file.txt`,
      'existing level1 content'
    );

    // Add some files to source that match existing files (duplicates)
    writeFileSync(
      `${sourceRoot}/duplicate_file.txt`,
      'source duplicate content'
    );
    writeFileSync(
      `${sourceRoot}/level1/duplicate_file.txt`,
      'source level1 duplicate content'
    );
    // Perform recursive copy (should ignore duplicates and not follow symlinks)
    await new Promise((resolve, reject) => {
      cp(sourceRoot, destRoot, { recursive: true, force: false }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    // Verify destination structure
    ok(existsSync(`${destRoot}/root_file.txt`));
    ok(existsSync(`${destRoot}/level1/level1_file.txt`));
    ok(existsSync(`${destRoot}/level1/level2/level2_file.txt`));
    ok(existsSync(`${destRoot}/level1/level2/level3/level3_file.txt`));
    ok(existsSync(`${destRoot}/level1/level2/level3/level4/level4_file.txt`));
    ok(
      existsSync(
        `${destRoot}/level1/level2/level3/level4/level5/level5_file.txt`
      )
    );

    // Verify file contents
    deepStrictEqual(
      readFileSync(`${destRoot}/root_file.txt`, 'utf8'),
      'root content'
    );
    deepStrictEqual(
      readFileSync(`${destRoot}/level1/level1_file.txt`, 'utf8'),
      'level1 content'
    );
    deepStrictEqual(
      readFileSync(`${destRoot}/level1/level2/level2_file.txt`, 'utf8'),
      'level2 content'
    );
    deepStrictEqual(
      readFileSync(`${destRoot}/level1/level2/level3/level3_file.txt`, 'utf8'),
      'level3 content'
    );
    deepStrictEqual(
      readFileSync(
        `${destRoot}/level1/level2/level3/level4/level4_file.txt`,
        'utf8'
      ),
      'level4 content'
    );
    deepStrictEqual(
      readFileSync(
        `${destRoot}/level1/level2/level3/level4/level5/level5_file.txt`,
        'utf8'
      ),
      'level5 content'
    );

    // Verify symlinks are copied as symlinks (not followed)
    ok(lstatSync(`${destRoot}/level1/level2/symlink_to_file`).isSymbolicLink());
    ok(
      lstatSync(
        `${destRoot}/level1/level2/level3/symlink_to_dir`
      ).isSymbolicLink()
    );

    // Verify duplicate files were ignored (original content preserved)
    deepStrictEqual(
      readFileSync(`${destRoot}/duplicate_file.txt`, 'utf8'),
      'existing content'
    );
    deepStrictEqual(
      readFileSync(`${destRoot}/level1/duplicate_file.txt`, 'utf8'),
      'existing level1 content'
    );

    // Verify existing directory structure remains
    ok(existsSync(`${destRoot}/existing_dir`));
  },
};

// Deep directory structure tests - promises version
export const deepDirectoryStructureTestsPromises = {
  async test() {
    const sourceRoot = '/tmp/deep_source_pr';
    const destRoot = '/tmp/deep_dest_pr';

    // Create source directory structure
    mkdirSync(sourceRoot, { recursive: true });
    mkdirSync(`${sourceRoot}/level1`, { recursive: true });
    mkdirSync(`${sourceRoot}/level1/level2`, { recursive: true });
    mkdirSync(`${sourceRoot}/level1/level2/level3`, { recursive: true });
    mkdirSync(`${sourceRoot}/level1/level2/level3/level4`, { recursive: true });
    mkdirSync(`${sourceRoot}/level1/level2/level3/level4/level5`, {
      recursive: true,
    });

    // Create files in various levels
    writeFileSync(`${sourceRoot}/root_file.txt`, 'root content');
    writeFileSync(`${sourceRoot}/level1/level1_file.txt`, 'level1 content');
    writeFileSync(
      `${sourceRoot}/level1/level2/level2_file.txt`,
      'level2 content'
    );
    writeFileSync(
      `${sourceRoot}/level1/level2/level3/level3_file.txt`,
      'level3 content'
    );
    writeFileSync(
      `${sourceRoot}/level1/level2/level3/level4/level4_file.txt`,
      'level4 content'
    );
    writeFileSync(
      `${sourceRoot}/level1/level2/level3/level4/level5/level5_file.txt`,
      'level5 content'
    );

    // Create symlinks (will not be followed)
    writeFileSync(`${sourceRoot}/link_target.txt`, 'link target content');
    symlinkSync(
      `${sourceRoot}/link_target.txt`,
      `${sourceRoot}/level1/level2/symlink_to_file`
    );
    symlinkSync(
      `${sourceRoot}/level1`,
      `${sourceRoot}/level1/level2/level3/symlink_to_dir`
    );

    // Create destination directory structure with some existing files (duplicates)
    mkdirSync(destRoot, { recursive: true });
    mkdirSync(`${destRoot}/level1`, { recursive: true });
    mkdirSync(`${destRoot}/level1/level2`, { recursive: true });
    mkdirSync(`${destRoot}/existing_dir`, { recursive: true });

    // Create files that will be duplicates (should be ignored)
    writeFileSync(`${destRoot}/duplicate_file.txt`, 'existing content');
    writeFileSync(
      `${destRoot}/level1/duplicate_file.txt`,
      'existing level1 content'
    );

    // Add some files to source that match existing files (duplicates)
    writeFileSync(
      `${sourceRoot}/duplicate_file.txt`,
      'source duplicate content'
    );
    writeFileSync(
      `${sourceRoot}/level1/duplicate_file.txt`,
      'source level1 duplicate content'
    );

    // Perform recursive copy (should ignore duplicates and not follow symlinks)
    await fsPromises.cp(sourceRoot, destRoot, {
      recursive: true,
      force: false,
    });

    // Verify destination structure
    ok(existsSync(`${destRoot}/root_file.txt`));
    ok(existsSync(`${destRoot}/level1/level1_file.txt`));
    ok(existsSync(`${destRoot}/level1/level2/level2_file.txt`));
    ok(existsSync(`${destRoot}/level1/level2/level3/level3_file.txt`));
    ok(existsSync(`${destRoot}/level1/level2/level3/level4/level4_file.txt`));
    ok(
      existsSync(
        `${destRoot}/level1/level2/level3/level4/level5/level5_file.txt`
      )
    );

    // Verify file contents
    deepStrictEqual(
      readFileSync(`${destRoot}/root_file.txt`, 'utf8'),
      'root content'
    );
    deepStrictEqual(
      readFileSync(`${destRoot}/level1/level1_file.txt`, 'utf8'),
      'level1 content'
    );
    deepStrictEqual(
      readFileSync(`${destRoot}/level1/level2/level2_file.txt`, 'utf8'),
      'level2 content'
    );
    deepStrictEqual(
      readFileSync(`${destRoot}/level1/level2/level3/level3_file.txt`, 'utf8'),
      'level3 content'
    );
    deepStrictEqual(
      readFileSync(
        `${destRoot}/level1/level2/level3/level4/level4_file.txt`,
        'utf8'
      ),
      'level4 content'
    );
    deepStrictEqual(
      readFileSync(
        `${destRoot}/level1/level2/level3/level4/level5/level5_file.txt`,
        'utf8'
      ),
      'level5 content'
    );

    // Verify symlinks are copied as symlinks (not followed)
    ok(lstatSync(`${destRoot}/level1/level2/symlink_to_file`).isSymbolicLink());
    ok(
      lstatSync(
        `${destRoot}/level1/level2/level3/symlink_to_dir`
      ).isSymbolicLink()
    );

    // Verify duplicate files were ignored (original content preserved)
    deepStrictEqual(
      readFileSync(`${destRoot}/duplicate_file.txt`, 'utf8'),
      'existing content'
    );
    deepStrictEqual(
      readFileSync(`${destRoot}/level1/duplicate_file.txt`, 'utf8'),
      'existing level1 content'
    );

    // Verify existing directory structure remains
    ok(existsSync(`${destRoot}/existing_dir`));
  },
};
