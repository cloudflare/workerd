from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    async def test(self, request, env):
        # Verify RPC methods whose names collide with Python's iterator/generator
        # protocol (send, next, throw, close) work through _FetcherWrapper.
        result = await self.env.SERVICE.send("hello")
        assert result == "sent:hello", f"send failed: {result!r}"

        result = await self.env.SERVICE.next("val")
        assert result == "next:val", f"next failed: {result!r}"

        result = await self.env.SERVICE.throw("err")
        assert result == "throw:err", f"throw failed: {result!r}"

        result = await self.env.SERVICE.close()
        assert result == "closed", f"close failed: {result!r}"

        result = await self.env.SERVICE.normalMethod("test")
        assert result == "normal:test", f"normalMethod failed: {result!r}"
