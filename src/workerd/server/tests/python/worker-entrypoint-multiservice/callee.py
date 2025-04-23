from workers import WorkerEntrypoint


class TestService(WorkerEntrypoint):
    def __init__(self, state, env):
        self.state = state
        self.counter = 0

    async def rpc_trigger_func(self):
        self.counter += 1
        return f"hello from python {self.counter}"

    async def test_function(self, message):
        return f"Received: {message}"
