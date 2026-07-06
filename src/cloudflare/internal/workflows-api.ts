// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export class NonRetryableError extends Error {
  constructor(message: string, name = 'NonRetryableError') {
    super(message);
    this.name = name;
  }
}

const workflowsBindingsRpc =
  !!Cloudflare.compatibilityFlags['workflows_bindings_rpc'];

interface Fetcher {
  fetch: typeof fetch;
  getInstance(id: string): Promise<{ id: string }>;
  create(options?: WorkflowInstanceCreateOptions): Promise<{ id: string }>;
  createBatch(
    options: WorkflowInstanceCreateOptions[]
  ): Promise<{ id: string }[]>;

  pause(id: string): Promise<void>;
  resume(id: string): Promise<void>;
  terminate(id: string, options?: WorkflowInstanceTerminateOptions): Promise<void>;
  restart(id: string, options?: WorkflowInstanceRestartOptions): Promise<void>;
  status(id: string): Promise<InstanceStatus>;
  sendEvent(
    id: string,
    event: { type: string; payload: unknown }
  ): Promise<void>;
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

class InstanceImpl implements WorkflowInstance {
  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private readonly fetcher: Fetcher;
  readonly id: string;

  constructor(id: string, fetcher: Fetcher) {
    this.id = id;
    this.fetcher = fetcher;
  }

  async pause(): Promise<void> {
    if (workflowsBindingsRpc) {
      await this.fetcher.pause(this.id);
      return;
    }
    await callFetcher(this.fetcher, '/pause', {
      id: this.id,
    });
  }
  async resume(): Promise<void> {
    if (workflowsBindingsRpc) {
      await this.fetcher.resume(this.id);
      return;
    }
    await callFetcher(this.fetcher, '/resume', {
      id: this.id,
    });
  }

  async terminate(options?: WorkflowInstanceTerminateOptions): Promise<void> {
    if (workflowsBindingsRpc) {
      await this.fetcher.terminate(this.id, options);
      return;
    }
    await callFetcher(this.fetcher, '/terminate', {
      id: this.id,
      ...(options?.rollback === true ? { rollback: true } : {}),
    });
  }

  async restart(options?: WorkflowInstanceRestartOptions): Promise<void> {
    if (workflowsBindingsRpc) {
      await this.fetcher.restart(this.id, options);
      return;
    }
    await callFetcher(this.fetcher, '/restart', {
      ...options,
      id: this.id,
    });
  }

  async status(): Promise<InstanceStatus> {
    if (workflowsBindingsRpc) {
      return await this.fetcher.status(this.id);
    }
    const result = await callFetcher<InstanceStatus>(this.fetcher, '/status', {
      id: this.id,
    });
    return result;
  }

  async sendEvent({
    type,
    payload,
  }: {
    type: string;
    payload: unknown;
  }): Promise<void> {
    if (workflowsBindingsRpc) {
      await this.fetcher.sendEvent(this.id, { type, payload });
      return;
    }
    await callFetcher(this.fetcher, '/send-event', {
      type,
      payload,
      id: this.id,
    });
  }
}

class WorkflowImpl {
  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private readonly fetcher: Fetcher;

  constructor(fetcher: Fetcher) {
    this.fetcher = fetcher;
  }

  async get(id: string): Promise<WorkflowInstance> {
    const result = workflowsBindingsRpc
      ? // getInstance, not get: avoids colliding with the built-in Fetcher.get(url).
        await this.fetcher.getInstance(id)
      : await callFetcher<{
          id: string;
        }>(this.fetcher, '/get', { id });

    return new InstanceImpl(result.id, this.fetcher);
  }

  async create(
    options?: WorkflowInstanceCreateOptions
  ): Promise<WorkflowInstance> {
    const result = workflowsBindingsRpc
      ? await this.fetcher.create(options)
      : await callFetcher<{
          id: string;
        }>(this.fetcher, '/create', options ?? {});

    return new InstanceImpl(result.id, this.fetcher);
  }

  async createBatch(
    options: WorkflowInstanceCreateOptions[]
  ): Promise<WorkflowInstance[]> {
    const results = workflowsBindingsRpc
      ? await this.fetcher.createBatch(options)
      : await callFetcher<
          {
            id: string;
          }[]
        >(this.fetcher, '/createBatch', options);

    return results.map((result) => new InstanceImpl(result.id, this.fetcher));
  }
}

export function makeBinding(env: { fetcher: Fetcher }): Workflow {
  return new WorkflowImpl(env.fetcher);
}

export default makeBinding;
