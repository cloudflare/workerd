// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/** @type {Array<VectorizeMatch>} */
const exampleVectorMatches = [
  {
    id: "b0daca4a-ffd8-4865-926b-e24800af2a2d",
    values: [0.2331, 1.0125, 0.6131, 0.9421, 0.9661, 0.8121],
    metadata: { text: "She sells seashells by the seashore" },
    score: 0.71151,
  },
  {
    id: "a44706aa-a366-48bc-8cc1-3feffd87d548",
    values: [0.2321, 0.8121, 0.6315, 0.6151, 0.4121, 0.1512],
    metadata: { text: "Peter Piper picked a peck of pickled peppers" },
    score: 0.68913,
  },
  {
    id: "43cfcb31-07e2-411f-8bf9-f82a95ba8b96",
    values: [0.0515, 0.7512, 0.8612, 0.2153, 0.15121, 0.6812],
    metadata: {
      text: "You know New York, you need New York, you know you need unique New York",
    },
    score: 0.94812,
  },
];
/** @type {Array<VectorizeVector>} */
const exampleVectors = exampleVectorMatches
  .filter((m) => typeof m !== "undefined")
  .map(({ id, values, metadata }) => ({
    id,
    values: values ?? [],
    metadata: metadata ?? {},
  }));

export default {
  /**
   * @param {Request} request
   */
  async fetch(request) {
    try {
      const { pathname } = new URL(request.url);

      if (request.method === "POST" && pathname.endsWith("/create")) {
        /** @type {VectorizeIndexConfig} */
        const config = await request.json();
        const name = pathname.split("/")[2];
        /** @type {VectorizeIndexDetails} */
        const index = {
          id: "ffeb30f5-d349-4ba5-8dde-79da543190fe",
          name: name || "my-index",
          config: config,
          vectorsCount: 0,
        };
        return Response.json(index);
      } else if (request.method === "GET" && pathname.endsWith("/list")) {
        /** @type {Array<VectorizeIndexDetails>} */
        const index = [
          {
            id: "0f48d520-5bf5-4980-acd9-98453fb8a27f",
            name: "my-first-index",
            config: {
              dimensions: 1536,
              preset: "openai/text-embedding-ada-002",
              metric: "euclidean",
            },
            vectorsCount: 500000,
          },
          {
            id: "b9fc84af-31f3-449c-bc61-b62abc86d5a1",
            name: "my-second-index",
            config: {
              dimensions: 1536,
              metric: "dot-product",
            },
            vectorsCount: 750000,
          },
        ];
        return Response.json(index);
      } else if (request.method === "GET" && pathname.split("/").length === 3) {
        /** @type {VectorizeIndexDetails} */
        const index = {
          id: "ffeb30f5-d349-4ba5-8dde-79da543190fe",
          name: pathname.split("/")[2] || "my-index",
          config: {
            dimensions: 1536,
            preset: "openai/text-embedding-ada-002",
            metric: "euclidean",
          },
          vectorsCount: 850850,
        };
        return Response.json(index);
      } else if (
        request.method === "DELETE" &&
        pathname.split("/").length === 2
      ) {
        return Response.json({});
      } else if (request.method === "POST" && pathname.endsWith("/query")) {
        /** @type {VectorizeQueryOptions & {vector: number[], compat: { queryMetadataOptional: boolean }}} */
        const body = await request.json();
        // check that the compatibility flags are set
        if (!body.compat.queryMetadataOptional)
          throw Error(
            "expected to get `queryMetadataOptional` compat flag with a value of true"
          );
        const returnSet = exampleVectorMatches;
        if (!body?.returnValues)
          returnSet.forEach((v) => {
            delete v.values;
          });
        if (!body?.returnMetadata)
          returnSet.forEach((v) => {
            delete v.metadata;
          });
        return Response.json({
          matches: returnSet,
          count: returnSet.length,
        });
      } else if (request.method === "POST" && pathname.endsWith("/insert")) {
        /** @type {{vectors: Array<VectorizeVector>}} */
        const data = await request.json();
        if (data.vectors.find((v) => v.id == "fail-with-test-error")) {
          return Response.json(
            {
              code: 9999,
              error: "You asked me for this error",
            },
            {
              status: 400,
            }
          );
        }

        return Response.json({
          ids: [
            ...data.vectors.map(({ id }) => id),
            ...exampleVectors.map(({ id }) => id),
          ],
          count: data.vectors.length + exampleVectors.length,
        });
      } else if (request.method === "POST" && pathname.endsWith("/upsert")) {
        /** @type {{vectors: Array<VectorizeVector>}} */
        let data = await request.json();
        if (data.vectors.length > 1) data.vectors.splice(-1);
        return Response.json({
          ids: [
            ...data.vectors.map(({ id }) => id),
            ...exampleVectors.map(({ id }) => id),
          ],
          count: data.vectors.length + exampleVectors.length,
        });
      } else if (
        request.method === "POST" &&
        pathname.endsWith("/deleteByIds")
      ) {
        /** @type {{ids: Array<string>}} */
        const body = await request.json();
        return Response.json({
          ids: body.ids,
          count: body.ids.length,
        });
      } else if (request.method === "POST" && pathname.endsWith("/getByIds")) {
        /** @type {{ids: Array<string>}} */
        const body = await request.json();
        return Response.json(
          exampleVectors.filter(({ id }) => body.ids.includes(id))
        );
      } else {
        return Response.json({ error: "Not found" }, { status: 404 });
      }
    } catch (err) {
      return Response.json(
        // @ts-ignore
        { error: err.message, stack: err.stack },
        { status: 500 }
      );
    }
  },
};
