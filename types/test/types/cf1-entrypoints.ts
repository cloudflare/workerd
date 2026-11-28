// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';

export class Validator extends WorkerEntrypoint implements DlpValidation {
  validate(input: DlpValidationInput): DlpValidationResult {
    return { score: input.entryId === 'trusted' ? 'veto' : 0.5 };
  }
}

export class AsyncValidator extends WorkerEntrypoint implements DlpValidation {
  async validate(_input: DlpValidationInput): Promise<DlpValidationResult> {
    return { score: 0 };
  }
}

// @ts-expect-error A validation entrypoint must implement `validate`.
export class MissingValidate extends WorkerEntrypoint implements DlpValidation {}

export class BadVerdict extends WorkerEntrypoint implements DlpValidation {
  validate(_input: DlpValidationInput): DlpValidationResult {
    // @ts-expect-error The score is a number or "veto".
    return { score: 'reject' };
  }
}

export class Middleware extends WorkerEntrypoint implements GatewayMiddleware {
  async handle(
    request: Request,
    context: GatewayMiddlewareContext,
    next: GatewayMiddlewareNext
  ): Promise<Response> {
    const identity: GatewayMiddlewareIdentity = await context.identity();
    const posture: GatewayMiddlewareDevicePosture =
      await context.devicePosture();
    if (identity.email === undefined || posture.checks.passed.length === 0) {
      return new Response('Forbidden', { status: 403 });
    }
    const upstream = await next(request);
    const response = new Response(upstream.body, upstream);
    response.headers.set('x-policy', context.policyId);
    return response;
  }
}

// @ts-expect-error A middleware entrypoint must implement `handle`.
export class MissingHandle extends WorkerEntrypoint implements GatewayMiddleware {}

export class SyncHandle extends WorkerEntrypoint implements GatewayMiddleware {
  // @ts-expect-error `handle` returns a Promise.
  handle(_request: Request): Response {
    return new Response();
  }
}

declare const context: GatewayMiddlewareContext;
// @ts-expect-error Context metadata is read-only.
context.host = 'example.com';
