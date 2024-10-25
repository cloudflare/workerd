// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { default as flags } from 'workerd:compatibility-flags';

interface Fetcher {
  fetch: typeof fetch;
}

enum Operation {
  INDEX_GET = 0,
  VECTOR_QUERY = 1,
  VECTOR_INSERT = 2,
  VECTOR_UPSERT = 3,
  VECTOR_GET = 4,
  VECTOR_DELETE = 5,
}

type VectorizeVersion = 'v1' | 'v2';

type QueryImplV2Params =
  | { vector: VectorFloatArray | number[]; vectorId?: undefined }
  | { vector?: undefined; vectorId: string };

function toNdJson(arr: object[]): string {
  return arr.reduce((acc, o) => acc + JSON.stringify(o) + '\n', '').trim();
}

/*
 * The Vectorize beta VectorizeIndex shares the same methods, so to keep things simple, they share one implementation.
 * The types here are specific to Vectorize GA, but the types here don't actually matter as they are stripped away
 * and not visible to end users.
 */
class VectorizeIndexImpl implements Vectorize {
  public constructor(
    private readonly fetcher: Fetcher,
    private readonly indexId: string,
    private readonly indexVersion: VectorizeVersion,
    private readonly useNdJson: boolean
  ) {}

  public async describe(): Promise<VectorizeIndexInfo> {
    const endpoint =
      this.indexVersion === 'v2' ? `info` : `binding/indexes/${this.indexId}`;
    const res = await this._send(Operation.INDEX_GET, endpoint, {
      method: 'GET',
    });

    return await toJson<VectorizeIndexInfo>(res);
  }

  public async query(
    vector: VectorFloatArray | number[],
    options?: VectorizeQueryOptions
  ): Promise<VectorizeMatches> {
    if (this.indexVersion === 'v2') {
      return await this.queryImplV2(
        { vector: Array.isArray(vector) ? vector : Array.from(vector) },
        options
      );
    } else {
      if (
        options &&
        options.returnMetadata &&
        typeof options.returnMetadata !== 'boolean'
      ) {
        throw new Error(
          `Invalid returnMetadata option. Expected boolean; got: ${options.returnMetadata}`
        );
      }
      const compat = {
        queryMetadataOptional: flags.vectorizeQueryMetadataOptional,
      };
      const res = await this._send(
        Operation.VECTOR_QUERY,
        `binding/indexes/${this.indexId}/query`,
        {
          method: 'POST',
          body: JSON.stringify({
            ...options,
            vector: Array.isArray(vector) ? vector : Array.from(vector),
            compat,
          }),
          headers: {
            'content-type': 'application/json',
            accept: 'application/json',
            'cf-vector-search-query-compat': JSON.stringify(compat),
          },
        }
      );

      return await toJson<VectorizeMatches>(res);
    }
  }

  public async queryById(
    vectorId: string,
    options?: VectorizeQueryOptions
  ): Promise<VectorizeMatches> {
    if (this.indexVersion === 'v1') {
      throw new Error(`QueryById operation is not supported for v1 indexes.`);
    } else {
      return await this.queryImplV2({ vectorId }, options);
    }
  }

  public async insert(
    vectors: VectorizeVector[]
  ): Promise<VectorizeAsyncMutation> {
    const endpoint =
      this.indexVersion === 'v2'
        ? `insert`
        : `binding/indexes/${this.indexId}/insert`;
    const bodyVecArr = vectors.map((vec) => ({
      ...vec,
      values: Array.isArray(vec.values) ? vec.values : Array.from(vec.values),
    }));

    const body = this.useNdJson
      ? toNdJson(bodyVecArr)
      : JSON.stringify({ vectors: bodyVecArr });

    const contentType = this.useNdJson
      ? 'application/x-ndjson'
      : 'application/json';

    const res = await this._send(Operation.VECTOR_INSERT, endpoint, {
      method: 'POST',
      body,
      headers: {
        'content-type': contentType,
        'cf-vector-search-dim-width': String(
          vectors.length ? vectors[0]?.values?.length : 0
        ),
        'cf-vector-search-dim-height': String(vectors.length),
        accept: 'application/json',
      },
    });

    return await toJson<VectorizeAsyncMutation>(res);
  }

  public async upsert(
    vectors: VectorizeVector[]
  ): Promise<VectorizeAsyncMutation> {
    const endpoint =
      this.indexVersion === 'v2'
        ? `upsert`
        : `binding/indexes/${this.indexId}/upsert`;
    const bodyVecArr = vectors.map((vec) => ({
      ...vec,
      values: Array.isArray(vec.values) ? vec.values : Array.from(vec.values),
    }));

    const body = this.useNdJson
      ? toNdJson(bodyVecArr)
      : JSON.stringify({ vectors: bodyVecArr });

    const contentType = this.useNdJson
      ? 'application/x-ndjson'
      : 'application/json';

    const res = await this._send(Operation.VECTOR_UPSERT, endpoint, {
      method: 'POST',
      body,
      headers: {
        'content-type': contentType,
        'cf-vector-search-dim-width': String(
          vectors.length ? vectors[0]?.values?.length : 0
        ),
        'cf-vector-search-dim-height': String(vectors.length),
        accept: 'application/json',
      },
    });

    return await toJson<VectorizeAsyncMutation>(res);
  }

  public async getByIds(ids: string[]): Promise<VectorizeVector[]> {
    const endpoint =
      this.indexVersion === 'v2'
        ? `getByIds`
        : `binding/indexes/${this.indexId}/getByIds`;
    const res = await this._send(Operation.VECTOR_GET, endpoint, {
      method: 'POST',
      body: JSON.stringify({ ids }),
      headers: {
        'content-type': 'application/json',
        accept: 'application/json',
      },
    });

    return await toJson<VectorizeVector[]>(res);
  }

  public async deleteByIds(ids: string[]): Promise<VectorizeAsyncMutation> {
    const endpoint =
      this.indexVersion === 'v2'
        ? `deleteByIds`
        : `binding/indexes/${this.indexId}/deleteByIds`;
    const res = await this._send(Operation.VECTOR_DELETE, endpoint, {
      method: 'POST',
      body: JSON.stringify({ ids }),
      headers: {
        'content-type': 'application/json',
        accept: 'application/json',
      },
    });

    return await toJson<VectorizeAsyncMutation>(res);
  }

  private async _send(
    operation: Operation,
    endpoint: string,
    init: RequestInit
  ): Promise<Response> {
    const res = await this.fetcher.fetch(
      `http://vector-search/${endpoint}`, // `http://vector-search` is just a dummy host, the attached fetcher will receive the request
      init
    );
    if (res.status !== 200) {
      let err: Error | null = null;

      try {
        const errResponse = (await res.json()) as VectorizeError;
        err = new Error(
          `${Operation[operation]}_ERROR${
            typeof errResponse.code === 'number'
              ? ` (code = ${errResponse.code})`
              : ''
          }: ${errResponse.error}`,
          {
            cause: new Error(errResponse.error),
          }
        );
      } catch {
        // do nothing
      }

      if (err) {
        throw err;
      } else {
        throw new Error(
          `${Operation[operation]}_ERROR: Status + ${res.status}`,
          {
            cause: new Error(`Status ${res.status}`),
          }
        );
      }
    }

    return res;
  }

  private async queryImplV2(
    vectorParams: QueryImplV2Params,
    options?: VectorizeQueryOptions
  ): Promise<VectorizeMatches> {
    if (options?.returnMetadata) {
      if (
        typeof options.returnMetadata !== 'boolean' &&
        !isVectorizeMetadataRetrievalLevel(options.returnMetadata)
      ) {
        throw new Error(
          `Invalid returnMetadata option. Expected: true, false, "none", "indexed" or "all"; got: ${options.returnMetadata}`
        );
      }

      if (typeof options.returnMetadata === 'boolean') {
        // Allow boolean returnMetadata for backward compatibility. true converts to 'all' and false converts to 'none'
        // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
        options.returnMetadata = options.returnMetadata ? 'all' : 'none';
      }
    }
    const res = await this._send(Operation.VECTOR_QUERY, `query`, {
      method: 'POST',
      body: JSON.stringify({
        ...options,
        ...(vectorParams.vector
          ? { vector: vectorParams.vector }
          : { vectorId: vectorParams.vectorId }),
      }),
      headers: {
        'content-type': 'application/json',
        accept: 'application/json',
      },
    });

    return await toJson<VectorizeMatches>(res);
  }
}

function isVectorizeMetadataRetrievalLevel(value: unknown): boolean {
  return (
    typeof value === 'string' &&
    (value === 'all' || value === 'indexed' || value === 'none')
  );
}

const maxBodyLogChars = 1_000;
async function toJson<T = unknown>(response: Response): Promise<T> {
  const body = await response.text();
  try {
    return JSON.parse(body) as T;
  } catch {
    throw new Error(
      `Failed to parse body as JSON, got: ${
        body.length > maxBodyLogChars
          ? `${body.slice(0, maxBodyLogChars)}…`
          : body
      }`
    );
  }
}

export function makeBinding(env: {
  fetcher: Fetcher;
  indexId: string;
  indexVersion?: VectorizeVersion;
  useNdJson?: boolean;
}): Vectorize {
  return new VectorizeIndexImpl(
    env.fetcher,
    env.indexId,
    env.indexVersion ?? 'v1',
    env.useNdJson ?? false
  );
}

export default makeBinding;
