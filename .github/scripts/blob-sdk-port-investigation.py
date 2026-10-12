from pathlib import Path


sdk = Path("sdk")
source = (sdk / "packages/miniflare/test/index.spec.ts").read_text()
start = source.index('test("Miniflare: dispatchConnect selects Worker TCP triggers"')
end = source.index('\ntest("Miniflare: dispatchConnect sockets are closed on dispose"', start)
test = source[start:end]
test = test.replace(
    'test("Miniflare: dispatchConnect selects Worker TCP triggers", async ({',
    'test.for(Array.from({ length: 51 }, (_, round) => round))(\n'
    '\t"TCP fixture port allocation investigation %i", async (round, {',
    1,
)
test = test.replace(
    "\tconst secondPort = await getPort({ exclude: [firstPort] });",
    """\tconst secondPort = await getPort({ exclude: [firstPort] });
\tconst forceCollision = round === 50;
\tconst nodeListeners: { address: string; port: number }[] = [];
\tconst originalListen = net.Server.prototype.listen;
\tlet forced = false;
\tconst listenSpy = vi.spyOn(net.Server.prototype, "listen").mockImplementation(
\t\tfunction (this: net.Server, ...args) {
\t\t\tconst forwarded: unknown[] = [...args];
\t\t\t// Model the OS assigning the released probe port to Miniflare's
\t\t\t// own loopback listener before workerd binds its TCP triggers.
\t\t\tif (forceCollision && !forced && forwarded[0] === 0) {
\t\t\t\tforwarded[0] = firstPort;
\t\t\t\tforced = true;
\t\t\t}
\t\t\tthis.once("listening", () => {
\t\t\t\tconst address = this.address();
\t\t\t\tif (address !== null && typeof address !== "string") {
\t\t\t\t\tnodeListeners.push({ address: address.address, port: address.port });
\t\t\t\t}
\t\t\t});
\t\t\treturn Reflect.apply(originalListen, this, forwarded);
\t\t}
\t);
\tonTestFinished(() => listenSpy.mockRestore());""",
    1,
)
test = test.replace(
    "\tonTestFinished(() => mf.dispose());",
    """\tonTestFinished(async () => {
\t\tif (forceCollision) {
\t\t\tawait expect(mf.dispose()).rejects.toThrow("Address already in use");
\t\t} else {
\t\t\tawait mf.dispose();
\t\t}
\t});
\tif (forceCollision) {
\t\tawait expect(mf.ready).rejects.toThrow("Address already in use");
\t\texpect(forced).toBe(true);
\t\texpect(nodeListeners.some(({ port }) => port === firstPort)).toBe(true);
\t\tconsole.info(JSON.stringify({ round, forceCollision, firstPort, secondPort, nodeListeners }));
\t\treturn;
\t}
\ttry {
\t\tawait mf.ready;
\t} catch (error) {
\t\tconsole.error(JSON.stringify({ round, forceCollision, firstPort, secondPort, nodeListeners }));
\t\tconsole.error(childProcess.execFileSync("ss", ["-ltnp"], { encoding: "utf8" }));
\t\tthrow error;
\t}
\tconsole.info(JSON.stringify({ round, forceCollision, firstPort, secondPort, nodeListeners }));""",
    1,
)
imports = """import childProcess from "node:child_process";
import net from "node:net";
import { text } from "node:stream/consumers";
import getPort from "get-port";
import { Miniflare } from "miniflare";
import { test, vi } from "vitest";
import { singleModuleManifest } from "./test-shared";

// CI-only diagnostics. The first 50 cases retain every original assertion;
// the final case deliberately forces a collision with Miniflare's loopback.
"""
(sdk / "packages/miniflare/test/tcp-port-investigation.spec.ts").write_text(imports + test)
