// Shared helper for running capnpc-capnp on .capnp schema files.
// Used by next-capnp-ordinal and compat-date-at tools.

import path from 'path';

/**
 * Run capnpc-capnp on a .capnp file and return the canonical text output.
 *
 * Automatically locates the bazel-built capnp tools, building them if needed.
 * Includes standard import paths for workerd's capnp files.
 *
 * @param root - Project root directory (ctx.worktree or ctx.directory)
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
  const importPath = path.join(
    root,
    'bazel-workerd/external/+http+capnp-cpp/src'
  );

  // Check tools exist, build if needed
  try {
    await Bun.$`test -x ${capnp} && test -x ${capnpcCapnp}`.quiet();
  } catch {
    try {
      await Bun.$`bazel build @capnp-cpp//src/capnp:capnp_tool @capnp-cpp//src/capnp:capnpc-capnp`
        .cwd(root)
        .quiet();
    } catch {
      return {
        error:
          'Could not build capnp tools. Run:\n  bazel build @capnp-cpp//src/capnp:capnp_tool @capnp-cpp//src/capnp:capnpc-capnp',
      };
    }
  }

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
    return await Bun.$`${capnp} compile ${importArgs} -o${capnpcCapnp} ${filePath}`
      .cwd(root)
      .text();
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
