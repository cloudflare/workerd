import assert from "node:assert";
import childProcess from "node:child_process";
import events from "node:events";
import fs from "node:fs/promises";
import path from "node:path";

const OUTPUT_PATH = "types/definitions";
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

function spawnWorkerd(configPath) {
  return new Promise((resolve) => {
    const workerdProcess = childProcess.spawn(
      "./src/workerd/server/workerd",
      ["serve", "--verbose", "--experimental", "--control-fd=3", configPath],
      { stdio: ["inherit", "inherit", "inherit", "pipe"] }
    );
    const exitPromise = events.once(workerdProcess, "exit");
    workerdProcess.stdio[3].on("data", (chunk) => {
      const message = JSON.parse(chunk.toString().trim());
      assert.strictEqual(message.event, "listen");
      resolve({
        url: new URL(`http://127.0.0.1:${message.port}`),
        async kill() {
          workerdProcess.kill("SIGINT");
          await exitPromise;
        },
      });
    });
  });
}

async function buildEntrypoint(entrypoint) {
  const url = new URL(`/${entrypoint.compatDate}.bundle`, worker.url);
  const response = await fetch(url);
  if (!response.ok) throw new Error(await response.text());
  const bundle = await response.formData();

  const name = entrypoint.name ?? entrypoint.compatDate;
  const entrypointPath = path.join(OUTPUT_PATH, name);
  await fs.mkdir(entrypointPath, { recursive: true });
  for (const [name, definitions] of bundle) {
    await fs.writeFile(path.join(entrypointPath, name), definitions);
  }
}

async function buildAllEntrypoints() {
  for (const entrypoint of ENTRYPOINTS) await buildEntrypoint(entrypoint);
}

const worker = await spawnWorkerd("./types/scripts/config.capnp");
try {
  await buildAllEntrypoints();
} finally {
  await worker.kill();
}
