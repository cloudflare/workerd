// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

export const handler: ExportedHandler<{ ARTIFACTS: Artifacts }> = {
  async fetch(_request, env) {
    const repos = await env.ARTIFACTS.list();
    expectType<ArtifactsRepoStatus>(repos.repos[0].status);

    const creating: ArtifactsRepoStatus = 'creating';
    expectType<ArtifactsRepoStatus>(creating);

    const errorCode: ArtifactsErrorCode = 'CREATE_IN_PROGRESS';
    expectType<ArtifactsErrorCode>(errorCode);

    // @ts-expect-error Artifacts repo list statuses must stay constrained.
    const unsupportedStatus: ArtifactsRepoStatus = 'provisioning';
    void unsupportedStatus;

    return new Response();
  },
};
