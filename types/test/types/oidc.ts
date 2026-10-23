// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';
import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';

declare const ctx: ExecutionContext;
declare const state: DurableObjectState;

expectTypeOf(ctx.oidc.getToken()).toEqualTypeOf<Promise<string>>();
expectTypeOf(
  ctx.oidc.getToken({ audience: 'urn:example:recipient' })
).toEqualTypeOf<Promise<string>>();
expectTypeOf(state.oidc.getToken()).toEqualTypeOf<Promise<string>>();
state.oidc.getToken({ subject: 'object' });
state.oidc.getToken({ subject: 'namespace', audience: 'https://api.example.com' });

// @ts-expect-error Only predefined subject modes may be selected.
state.oidc.getToken({ subject: 'worker:forged' });
// @ts-expect-error Workload identity is supplied by the host.
ctx.oidc.getToken({ workerId: 'forged' });

class IdentityObject extends DurableObject {
  token(): Promise<string> {
    return this.ctx.oidc.getToken({ subject: 'namespace' });
  }
}

class IdentityService extends WorkerEntrypoint {
  token(): Promise<string> {
    return this.ctx.oidc.getToken();
  }
}
