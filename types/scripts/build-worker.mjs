import fs from "node:fs/promises";
import module from "node:module";
import path from "node:path";
import url from "node:url";
import { build } from "esbuild";

async function readPath(rootPath) {
  try {
    return await fs.readFile(rootPath, "utf8");
  } catch (e) {
    if (e.code !== "EISDIR") throw e;
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
  const data = await fs.readFile("src/workerd/tools/param-names.json", "utf8");
  const recordArray = JSON.parse(data);

  const result = {};
  for (const record of recordArray) {
    const structureName = record.fully_qualified_parent_name
      .filter(Boolean)
      .join("::");

    let functionName = record.function_like_name;
    if (functionName.endsWith("_")) functionName = functionName.slice(0, -1);
    // `constructor` is a reserved property name
    if (functionName === "constructor") functionName = `$${functionName}`;

    const structureRecord = (result[structureName] ??= {});
    const functionArray = (structureRecord[functionName] ??= []);
    functionArray[record.index] = record.name;
  }
  return result;
}

await build({
  logLevel: "info",
  format: "esm",
  target: "esnext",
  external: ["node:*", "workerd:*"],
  bundle: true,
  // TODO(soon): enable minification before deploying worker
  // minify: true,
  outdir: "types/dist",
  outExtension: { ".js": ".mjs" },
  entryPoints: ["types/src/worker/index.mjs"],
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
            const contents = await fs.readFile(
              "types/types_internal/comments.json",
              "utf8"
            );
            return { contents, loader: "json" };
          }
        });
      },
    },
  ],
});
