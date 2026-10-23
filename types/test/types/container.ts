// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';

// ExecProcess.stdin/stdout/stderr are jsg::Optional getters: a stream the
// process does not provide is `undefined`, never `null`. That is the common
// case: stdin without `stdin: "pipe"`, stdout with `stdout: "ignore"`, stderr
// with `stderr: "ignore"` or `"combined"` (so also with a PTY).
expectTypeOf<ExecProcess['stdin']>().toEqualTypeOf<
  WritableStream | undefined
>();
expectTypeOf<ExecProcess['stdout']>().toEqualTypeOf<
  ReadableStream | undefined
>();
expectTypeOf<ExecProcess['stderr']>().toEqualTypeOf<
  ReadableStream | undefined
>();

export async function readExecStreams(container: Container) {
  const proc = await container.exec(['sh', '-lc', 'echo out; echo err >&2'], {
    stderr: 'combined',
  });
  expectTypeOf(proc.stdin).toEqualTypeOf<WritableStream | undefined>();
  expectTypeOf(proc.stdout).toEqualTypeOf<ReadableStream | undefined>();
  expectTypeOf(proc.stderr).toEqualTypeOf<ReadableStream | undefined>();

  // Truthiness is the documented way to test for a stream.
  if (proc.stdout) {
    expectTypeOf(proc.stdout).toEqualTypeOf<ReadableStream>();
  }
  if (proc.stdin) {
    expectTypeOf(proc.stdin).toEqualTypeOf<WritableStream>();
  }
  return proc.stderr ?? proc.stdout;
}
