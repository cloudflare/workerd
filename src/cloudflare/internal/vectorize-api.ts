// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as flags from 'workerd:compatibility-flags'

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

class VectorizeIndexImpl implements VectorizeIndex {
  public constructor(
    private readonly fetcher: Fetcher,
    private readonly indexId: string
  ) { }

  public async describe(): Promise<VectorizeIndexDetails> {
    const res = await this._send(
      Operation.INDEX_GET,
      `indexes/${this.indexId}`,
      {
        method: "GET",
      }
    );

    return await toJson<VectorizeIndexDetails>(res);
  }

  public async query(
    vector: VectorFloatArray | number[],
    options: VectorizeQueryOptions
  ): Promise<VectorizeMatches> {
    const res = await this._send(
      Operation.VECTOR_QUERY,
      `indexes/${this.indexId}/query`,
      {
        method: "POST",
        body: JSON.stringify({
          ...options,
          vector: Array.isArray(vector) ? vector : Array.from(vector),
          compat: {
            queryMetadataOptional: !!flags.vectorizeQueryMetadataOptional,
          },
        }),
        headers: {
          "content-type": "application/json",
          accept: "application/json",
          "cf-vector-search-query-compat": JSON.stringify({
            queryMetadataOptional: !!flags.vectorizeQueryMetadataOptional,
          })
        },
      }
    );

    return await toJson<VectorizeMatches>(res);
  }

  public async insert(
    vectors: VectorizeVector[]
  ): Promise<VectorizeVectorMutation> {
    const res = await this._send(
      Operation.VECTOR_INSERT,
      `indexes/${this.indexId}/insert`,
      {
        method: "POST",
        body: JSON.stringify({
          vectors: vectors.map((vec) => ({
            ...vec,
            values: Array.isArray(vec.values)
              ? vec.values
              : Array.from(vec.values),
          })),
        }),
        headers: {
          "content-type": "application/json",
          "cf-vector-search-dim-width": String(
            vectors.length ? vectors[0]?.values?.length : 0
          ),
          "cf-vector-search-dim-height": String(vectors.length),
          accept: "application/json",
        },
      }
    );

    return await toJson<VectorizeVectorMutation>(res);
  }

  public async upsert(
    vectors: VectorizeVector[]
  ): Promise<VectorizeVectorMutation> {
    const res = await this._send(
      Operation.VECTOR_UPSERT,
      `indexes/${this.indexId}/upsert`,
      {
        method: "POST",
        body: JSON.stringify({
          vectors: vectors.map((vec) => ({
            ...vec,
            values: Array.isArray(vec.values)
              ? vec.values
              : Array.from(vec.values),
          })),
        }),
        headers: {
          "content-type": "application/json",
          "cf-vector-search-dim-width": String(
            vectors.length ? vectors[0]?.values?.length : 0
          ),
          "cf-vector-search-dim-height": String(vectors.length),
          accept: "application/json",
        },
      }
    );

    return await toJson<VectorizeVectorMutation>(res);
  }

  public async getByIds(ids: string[]): Promise<VectorizeVector[]> {
    const res = await this._send(
      Operation.VECTOR_GET,
      `indexes/${this.indexId}/getByIds`,
      {
        method: "POST",
        body: JSON.stringify({ ids }),
        headers: {
          "content-type": "application/json",
          accept: "application/json",
        },
      }
    );

    return await toJson<VectorizeVector[]>(res);
  }

  public async deleteByIds(ids: string[]): Promise<VectorizeVectorMutation> {
    const res = await this._send(
      Operation.VECTOR_DELETE,
      `indexes/${this.indexId}/deleteByIds`,
      {
        method: "POST",
        body: JSON.stringify({ ids }),
        headers: {
          "content-type": "application/json",
          accept: "application/json",
        },
      }
    );

    return await toJson<VectorizeVectorMutation>(res);
  }

  private async _send(
    operation: Operation,
    endpoint: string,
    init: RequestInit
  ): Promise<Response> {
    const res = await this.fetcher.fetch(
      `http://vector-search/binding/${endpoint}`, // `http://vector-search` is just a dummy host, the attached fetcher will receive the request
      init
    );
    if (res.status !== 200) {
      let err: Error | null = null;

      try {
        const errResponse = (await res.json()) as VectorizeError;
        err = new Error(
          `${Operation[operation]}_ERROR ${typeof errResponse.code === "number" ? `(code = ${errResponse.code})` : ""
          }: ${errResponse.error}`,
          {
            cause: new Error(errResponse.error),
          }
        );
      } catch (e) { }

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
}

const maxBodyLogChars = 1_000;
async function toJson<T = unknown>(response: Response): Promise<T> {
  const body = await response.text();
  try {
    return JSON.parse(body) as T;
  } catch (e) {
    throw new Error(
      `Failed to parse body as JSON, got: ${body.length > maxBodyLogChars
        ? `${body.slice(0, maxBodyLogChars)}…`
        : body
      }`
    );
  }
}

export function makeBinding(env: {
  fetcher: Fetcher;
  indexId: string;
}): VectorizeIndex {
  return new VectorizeIndexImpl(env.fetcher, env.indexId);
}

export default makeBinding;
