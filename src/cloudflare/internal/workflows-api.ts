// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export class NonRetryableError extends Error {
  // `__brand` is how engine validates that the user returned a `NonRetryableError`
  // imported from "cloudflare:workflows"
  // This enables them to extend NonRetryableError for their own Errors
  // as well by overriding name
  // Private fields are not serialized over RPC
  public readonly __brand: string = 'NonRetryableError';

  public constructor(message: string, name = 'NonRetryableError') {
    super(message);
    this.name = name;
  }
}

interface Fetcher {
  fetch: typeof fetch;
}

async function callFetcher<T>(
  fetcher: Fetcher,
  path: string,
  body: object
): Promise<T> {
  const res = await fetcher.fetch(`http://workflow-binding.local${path}`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'X-Version': '1',
    },
    body: JSON.stringify(body),
  });

  const response = (await res.json()) as {
    result: T;
    error?: WorkflowError;
  };

  if (res.ok) {
    return response.result;
  } else {
    throw new Error(response.error?.message);
  }
}

class InstanceImpl implements Instance {
  private readonly fetcher: Fetcher;
  public readonly id: string;

  public constructor(id: string, fetcher: Fetcher) {
    this.id = id;
    this.fetcher = fetcher;
  }

  public async pause(): Promise<void> {
    await callFetcher(this.fetcher, '/pause', {
      id: this.id,
    });
  }
  public async resume(): Promise<void> {
    await callFetcher(this.fetcher, '/resume', {
      id: this.id,
    });
  }

  public async terminate(): Promise<void> {
    await callFetcher(this.fetcher, '/terminate', {
      id: this.id,
    });
  }

  public async restart(): Promise<void> {
    await callFetcher(this.fetcher, '/restart', {
      id: this.id,
    });
  }

  public async status(): Promise<InstanceStatus> {
    const result = await callFetcher<InstanceStatus>(this.fetcher, '/status', {
      id: this.id,
    });
    return result;
  }
}

class WorkflowImpl {
  private readonly fetcher: Fetcher;

  public constructor(fetcher: Fetcher) {
    this.fetcher = fetcher;
  }

  public async get(id: string): Promise<Instance> {
    const result = await callFetcher<{ instanceId: string }>(
      this.fetcher,
      '/get',
      { id }
    );

    return new InstanceImpl(result.instanceId, this.fetcher);
  }

  public async create(id: string, params?: unknown): Promise<Instance> {
    const result = await callFetcher<{ instanceId: string }>(
      this.fetcher,
      '/create',
      { id, params }
    );

    return new InstanceImpl(result.instanceId, this.fetcher);
  }
}

export function makeBinding(env: { fetcher: Fetcher }): Workflow {
  return new WorkflowImpl(env.fetcher);
}

export default makeBinding;
