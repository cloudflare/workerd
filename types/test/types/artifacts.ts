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

async function testRepositoryDisposal(): Promise<void> {
  // The repository capability is an RPC stub and supports explicit resource management.
  using repo = await artifacts.get('example');
  expectType<ArtifactsRepoInfo>(await repo.info());
}

async function testCreateResult(): Promise<void> {
  const created = await artifacts.create('example');
  expectType<string>(created.token);
}

function testErrorCodes(code: ArtifactsErrorCode): void {
  switch (code) {
    case 'CREATE_IN_PROGRESS':
    case 'IMPORT_IN_PROGRESS':
    case 'FORK_IN_PROGRESS':
      return;
  }
}

void testRepositoryReads;
void testRepositoryDisposal;
void testCreateResult;
void testErrorCodes;

async function testSnapshotDiff(): Promise<void> {
  using repo = await artifacts.get('example');
  expectType<ArtifactsDiffResult>(await repo.diff('main', 'v1'));
  const page = await repo.diff(null, 'a'.repeat(40), { limit: 1 });
  expectType<string | null>(page.from);
  expectType<string>(page.to);
  expectType<ArtifactsDiffEntry[]>(page.changes);
  expectType<string | undefined>(page.cursor);
  expectType<ArtifactsDiffResult>(
    await repo.diff(page.from, page.to, { cursor: page.cursor, limit: 1000 })
  );
  for (const entry of page.changes) {
    expectType<'added' | 'modified' | 'deleted' | 'type_changed'>(entry.status);
    expectType<string | undefined>(entry.oldMode);
    expectType<string | undefined>(entry.newMode);
    expectType<string | undefined>(entry.oldHash);
    expectType<string | undefined>(entry.newHash);
  }
  // @ts-expect-error The destination cannot be null.
  await repo.diff('main', null);
  // @ts-expect-error The base is required, even when empty.
  await repo.diff('main');
  // @ts-expect-error No numeric offset pagination.
  await repo.diff(null, 'main', { offset: 1 });
  // @ts-expect-error No total count is calculated.
  page.total;
}
