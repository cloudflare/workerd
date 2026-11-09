// Shared helper for running capnpc-capnp on .capnp schema files.
// Used by next-capnp-ordinal and compat-date-at tools.

import { execFile } from 'node:child_process';
import { access, constants, realpath } from 'node:fs/promises';
import path from 'node:path';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);

/**
 * Run capnpc-capnp on a .capnp file and return the canonical text output.
 *
 * Automatically locates the bazel-built capnp tools, building them if needed.
 * Includes standard import paths for workerd's capnp files.
 *
 * @param root - Project root directory (the workerd checkout)
 * @param filePath - Absolute path to the .capnp file
 * @returns The canonical schema text, or an error string prefixed with "Error:"
 */
export async function runCapnpcCapnp(
  root: string,
  filePath: string
): Promise<string | { error: string }> {
  const capnp = path.join(
    root,
    'bazel-bin/external/+http+capnp-cpp/src/capnp/capnp_tool'
  );
  const capnpcCapnp = path.join(
    root,
    'bazel-bin/external/+http+capnp-cpp/src/capnp/capnpc-capnp'
  );

  // Check tools exist, build if needed
  try {
    await access(capnp, constants.X_OK);
    await access(capnpcCapnp, constants.X_OK);
  } catch {
    try {
      await execFileAsync(
        'bazel',
        [
          'build',
          '@capnp-cpp//src/capnp:capnp_tool',
          '@capnp-cpp//src/capnp:capnpc-capnp',
        ],
        { cwd: root, maxBuffer: 64 * 1024 * 1024 }
      );
    } catch {
      return {
        error:
          'Could not build capnp tools. Run:\n  bazel build @capnp-cpp//src/capnp:capnp_tool @capnp-cpp//src/capnp:capnpc-capnp',
      };
    }
  }

  // The execroot's convenience symlink is named after the checkout directory (`bazel-workerd`,
  // or `bazel-<dir>` in a worktree), so reach the execroot through `bazel-out` instead.
  let execroot: string;
  try {
    execroot = path.dirname(await realpath(path.join(root, 'bazel-out')));
  } catch {
    return {
      error:
        'Could not locate the bazel execroot: the bazel-out symlink in the project root is ' +
        'missing. Run a bazel build (without --symlink_prefix or --experimental_convenience_symlinks=ignore) first.',
    };
  }
  const importPath = path.join(execroot, 'external/+http+capnp-cpp/src');

  const fileDir = path.dirname(filePath);
  const importArgs = [
    '--no-standard-import',
    `-I${importPath}`,
    `-I${fileDir}`,
    `-I${path.join(root, 'src/workerd/io')}`,
    `-I${path.join(root, 'src/workerd/server')}`,
    `-I${path.join(root, 'src')}`,
  ];

  try {
    const { stdout } = await execFileAsync(
      capnp,
      ['compile', ...importArgs, `-o${capnpcCapnp}`, filePath],
      { cwd: root, maxBuffer: 64 * 1024 * 1024 }
    );
    return stdout;
  } catch (e: any) {
    return { error: `capnpc-capnp failed:\n${e.stderr || e.message}` };
  }
}

/**
 * Resolve a .capnp file path (absolute or relative to project root).
 */
export function resolveCapnpPath(root: string, file: string): string {
  return path.isAbsolute(file) ? file : path.join(root, file);
}
