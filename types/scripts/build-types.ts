import assert from "node:assert";
import childProcess from "node:child_process";
import events from "node:events";
import { readFileSync, readdirSync } from "node:fs";
import fs from "node:fs/promises";
import path from "node:path";
import prettier from "prettier";
import ts from "typescript";
import { SourcesMap, createMemoryProgram } from "../src/program";
import { getFilePath } from "../src/utils";

const OUTPUT_PATH = getFilePath("types/definitions");
const ENTRYPOINTS = [
  { compatDate: "2021-01-01", name: "oldest" },
  // https://developers.cloudflare.com/workers/platform/compatibility-dates/#formdata-parsing-supports-file
  { compatDate: "2021-11-03" },
  // https://developers.cloudflare.com/workers/platform/compatibility-dates/#settersgetters-on-api-object-prototypes
  { compatDate: "2022-01-31" },
  // https://developers.cloudflare.com/workers/platform/compatibility-dates/#global-navigator
  { compatDate: "2022-03-21" },
  // https://developers.cloudflare.com/workers/platform/compatibility-dates/#r2-bucket-list-respects-the-include-option
  { compatDate: "2022-08-04" },
  // https://developers.cloudflare.com/workers/platform/compatibility-dates/#new-url-parser-implementation
  { compatDate: "2022-10-31" },
  // https://developers.cloudflare.com/workers/platform/compatibility-dates/#streams-constructors
  // https://developers.cloudflare.com/workers/platform/compatibility-dates/#compliant-transformstream-constructor
  { compatDate: "2022-11-30" },
  // https://github.com/cloudflare/workerd/blob/fcb6f33d10c71975cb2ce68dbf1924a1eeadbd8a/src/workerd/io/compatibility-date.capnp#L275-L280 (http_headers_getsetcookie)
  { compatDate: "2023-03-01" },
  // https://github.com/cloudflare/workerd/blob/fcb6f33d10c71975cb2ce68dbf1924a1eeadbd8a/src/workerd/io/compatibility-date.capnp#L307-L312 (urlsearchparams_delete_has_value_arg)
  { compatDate: "2023-07-01" },
  // Latest compatibility date with experimental features
  { compatDate: "experimental" },
];

/**
 * Copy all TS lib files into the memory filesystem. We only use lib.esnext
 * but since TS lib files all reference each other to various extents it's
 * easier to add them all and let TS figure out which ones it actually needs to load.
 * This function uses the current local installation of TS as a source for lib files
 */
function loadLibFiles(): SourcesMap {
  const libLocation = path.dirname(require.resolve("typescript"));
  const libFiles = readdirSync(libLocation).filter(
    (file) => file.startsWith("lib.") && file.endsWith(".d.ts")
  );
  const lib: SourcesMap = new Map();
  for (const file of libFiles) {
    lib.set(
      `/node_modules/typescript/lib/${file}`,
      readFileSync(path.join(libLocation, file), "utf-8")
    );
  }
  return lib;
}

function checkDiagnostics(sources: SourcesMap): void {
  const program = createMemoryProgram(
    sources,
    undefined,
    {
      noEmit: true,
      lib: ["lib.esnext.d.ts"],
      types: [],
    },
    loadLibFiles()
  );

  const emitResult = program.emit();

  const allDiagnostics = ts
    .getPreEmitDiagnostics(program)
    .concat(emitResult.diagnostics);

  allDiagnostics.forEach((diagnostic) => {
    if (diagnostic.file) {
      const { line, character } = ts.getLineAndCharacterOfPosition(
        diagnostic.file,
        diagnostic.start!
      );
      const message = ts.flattenDiagnosticMessageText(
        diagnostic.messageText,
        "\n"
      );
      console.log(
        `${diagnostic.file.fileName}:${line + 1}:${character + 1} : ${message}`
      );
    } else {
      console.log(
        ts.flattenDiagnosticMessageText(diagnostic.messageText, "\n")
      );
    }
  });

  assert(allDiagnostics.length === 0, "TypeScript failed to compile!");
}

function spawnWorkerd(
  configPath: string
): Promise<{ url: URL; kill: () => Promise<void> }> {
  return new Promise((resolve) => {
    const workerdProcess = childProcess.spawn(
      getFilePath("src/workerd/server/workerd"),
      ["serve", "--verbose", "--experimental", "--control-fd=3", configPath],
      { stdio: ["inherit", "inherit", "inherit", "pipe"] }
    );
    const exitPromise = events.once(workerdProcess, "exit");
    workerdProcess.stdio[3]?.on("data", (chunk: Buffer): void => {
      const message = JSON.parse(chunk.toString().trim()) as {
        event: string
        port: number
      };
      assert.strictEqual(message.event, "listen");
      resolve({
        url: new URL(`http://127.0.0.1:${message.port}`),
        async kill() {
          workerdProcess.kill("SIGTERM");
          await exitPromise;
        },
      });
    });
  });
}

async function buildEntrypoint(
  entrypoint: (typeof ENTRYPOINTS)[number],
  workerUrl: URL
): Promise<void> {
  const url = new URL(`/${entrypoint.compatDate}.bundle`, workerUrl);
  const response = await fetch(url);
  if (!response.ok) throw new Error(await response.text());
  const bundle = await response.formData();

  const name = entrypoint.name ?? entrypoint.compatDate;
  const entrypointPath = path.join(OUTPUT_PATH, name);
  await fs.mkdir(entrypointPath, { recursive: true });
  for (const [name, definitions] of bundle) {
    assert(typeof definitions === "string");
    const prettierIgnoreRegexp = /^\s*\/\/\s*prettier-ignore\s*\n/gm;
    let typings = definitions.replaceAll(prettierIgnoreRegexp, "");

    typings = await prettier.format(typings, {
      parser: "typescript",
    });

    checkDiagnostics(new SourcesMap([["/$virtual/source.ts", typings]]));

    await fs.writeFile(path.join(entrypointPath, name), typings);
  }
}

async function buildAllEntrypoints(workerUrl: URL): Promise<void> {
  for (const entrypoint of ENTRYPOINTS)
    await buildEntrypoint(entrypoint, workerUrl);
}
export async function main(): Promise<void> {
  const worker = await spawnWorkerd(getFilePath("types/scripts/config.capnp"));
  try {
    await buildAllEntrypoints(worker.url);
  } finally {
    await worker.kill();
  }
}

// Outputting to a CommonJS module so can't use top-level await
if (require.main === module) void main();
