from workers import WorkerEntrypoint, import_from_javascript


class TestService(WorkerEntrypoint):
    async def test_function(self, message):
        return f"Received: {message}"

    async def test_import(self):
        # Import a JavaScript module from the RPC target
        node_assert = import_from_javascript("node:assert")
        assert hasattr(node_assert, "strictEqual"), "strictEqual function should exist"

        # Import the env module and test it works in the RPC context
        cloudflare_workers = import_from_javascript("cloudflare:workers")
        env = cloudflare_workers.env

        # Verify the environment binding is correctly accessible
        if env.TEST_VALUE == "TEST_IN_RPC":
            return "Import worked in RPC context"
        else:
            return f"Import failed or wrong value: {env.TEST_VALUE}"


def test():
    from js import console

    console.log("RPC callee test function executed successfully")
    # Nothing to test directly here, since this is called via RPC
