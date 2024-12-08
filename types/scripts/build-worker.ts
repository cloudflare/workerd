import assert from "node:assert";
import fs from "node:fs/promises";
import path from "node:path";
import { build } from "esbuild";
import { CommentsData } from "src/transforms";
import cloudflareComments from "../src/cloudflare";
import { collateStandardComments } from "../src/standards";
import { getFilePath } from "../src/utils";

async function readPath(rootPath: string): Promise<string> {
  try {
    return await fs.readFile(rootPath, "utf8");
  } catch (e) {
    if (!(e && typeof e === "object" && "code" in e && e.code === "EISDIR"))
      throw e;
    const fileNames = await fs.readdir(rootPath);
    const contentsPromises = fileNames.map((fileName) => {
      const filePath = path.join(rootPath, fileName);
      return readPath(filePath);
    });
    const contents = await Promise.all(contentsPromises);
    return contents.join("\n");
  }
}

async function readParamNames() {
  // Support methods defined in parent classes
  const additionalClassNames: [string, string][] = [
    ["DurableObjectStorageOperations", "DurableObjectStorage"],
    ["DurableObjectStorageOperations", "DurableObjectTransaction"],
  ];

  const data = await fs.readFile(getFilePath("src/workerd/tools/param-names.json"), "utf8");
  const recordArray = JSON.parse(data) as {
    fully_qualified_parent_name: string[];
    function_like_name: string;
    index: number;
    name: string;
  }[];

  function registerApi(
    structureName: string,
    record: (typeof recordArray)[number]
  ) {
    let functionName: string = record.function_like_name;
    if (functionName.endsWith("_")) functionName = functionName.slice(0, -1);
    // `constructor` is a reserved property name
    if (functionName === "constructor") functionName = `$${functionName}`;

    result[structureName] ??= {};
    const structureRecord = result[structureName];

    structureRecord[functionName] ??= [];
    const functionArray = structureRecord[functionName];
    functionArray[record.index] = record.name;
  }

  const result: Record<string, Record<string, string[]>> = {};
  for (const record of recordArray) {
    const structureName: string = record.fully_qualified_parent_name
      .filter(Boolean)
      .join("::");

    registerApi(structureName, record);

    for (const [className, renamedClass] of additionalClassNames) {
      if (structureName.includes(className)) {
        registerApi(structureName.replace(className, renamedClass), record);
      }
    }
  }
  return result;
}

export async function readComments(): Promise<CommentsData> {
  const comments = collateStandardComments(
    path.join(
      path.dirname(require.resolve("typescript")),
      "lib.webworker.d.ts"
    ),
    path.join(
      path.dirname(require.resolve("typescript")),
      "lib.webworker.iterable.d.ts"
    )
  );

  // We want to deep merge here so that our comment overrides can be very targeted
  for (const [name, members] of Object.entries(cloudflareComments)) {
    comments[name] ??= {};
    for (const [member, comment] of Object.entries(members)) {
      const apiEntry = comments[name];
      assert(apiEntry !== undefined);
      apiEntry[member] = comment;
    }
  }
  return comments;
}

if (require.main === module)
  void build({
    logLevel: "info",
    format: "esm",
    target: "esnext",
    external: ["node:*", "workerd:*"],
    bundle: true,
    minify: true,
    outdir: getFilePath("types/dist"),
    outExtension: { ".js": ".mjs" },
    entryPoints: [getFilePath("types/src/worker/index.ts")],
    plugins: [
      {
        name: "raw",
        setup(build) {
          build.onResolve({ filter: /^raw:/ }, async (args) => {
            const resolved = path.resolve(args.resolveDir, args.path.slice(4));
            return { namespace: "raw", path: resolved };
          });
          build.onLoad({ namespace: "raw", filter: /.*/ }, async (args) => {
            const contents = await readPath(args.path);
            return { contents, loader: "text" };
          });
        },
      },
      {
        name: "virtual",
        setup(build) {
          build.onResolve({ filter: /^virtual:/ }, (args) => {
            return {
              namespace: "virtual",
              path: args.path.substring("virtual:".length),
            };
          });
          build.onLoad({ namespace: "virtual", filter: /.*/ }, async (args) => {
            if (args.path === "param-names.json") {
              const contents = await readParamNames();
              return { contents: JSON.stringify(contents), loader: "json" };
            }
            if (args.path === "comments.json") {
              const comments = await readComments();
              return { contents: JSON.stringify(comments), loader: "json" };
            }
          });
        },
      },
    ],
  });
