// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

declare const artifacts: Artifacts;

async function testRepositoryReads(): Promise<void> {
  const repo = await artifacts.get('example');

  expectType<ArtifactsRepoInfo>(await repo.info());
  expectType<Blob | null>(await repo.readBlob('0'.repeat(40)));
  expectType<ArtifactsTreeEntry[] | null>(await repo.readTree('0'.repeat(40)));
  expectType<ArtifactsCommitMetadata | null>(
    await repo.readCommit('0'.repeat(40))
  );
  expectType<Blob | null>(
    await repo.readFile({ ref: 'main', path: 'README.md' })
  );
  expectType<ArtifactsCommitMetadata[]>(
    await repo.log({ ref: 'main', limit: 10, offset: 0 })
  );

  // Repository metadata requires a fresh lookup through info().
  // @ts-expect-error ArtifactsRepo does not expose metadata properties.
  repo.name;
}

void testRepositoryReads;
