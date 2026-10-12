from pathlib import Path

source = Path("sdk/packages/miniflare/test/index.spec.ts").read_text()
start = source.index('test("Miniflare: dispatchConnect selects Worker TCP triggers"')
end = source.index('test("Miniflare: dispatchConnect sockets are closed on dispose"', start)
fixture = source[start:end].replace(
    'test("Miniflare: dispatchConnect selects Worker TCP triggers", async ({',
    'test.for(Array.from({ length: 50 }, (_, round) => round))(\n'
    '"Miniflare: TCP trigger fixture repetition %i", async (_round, {',
    1,
)
imports = '''import { text } from "node:stream/consumers";
import { Miniflare } from "miniflare";
import { test } from "vitest";
import { singleModuleManifest } from "./test-shared";

'''
Path("sdk/packages/miniflare/test/tcp-port-fix-repeated.spec.ts").write_text(
    imports + fixture
)

selection = Path("sdk/packages/miniflare/test/connect.spec.ts").read_text()
selection = selection.replace(
    "test.for([", "test.for(Array.from({ length: 5 }, () => [", 1
).replace(
    "] as const)(", "] as const).flat())(", 1
).replace(
    'test("selectConnectTrigger: defaults to TCP", ({ expect }) => {',
    'test.for(Array.from({ length: 5 }, (_, round) => round))(\n'
    '"selectConnectTrigger: TCP default repetition %i", (_round, { expect }) => {',
    1,
)
Path("sdk/packages/miniflare/test/connect-fix-repeated.spec.ts").write_text(selection)
