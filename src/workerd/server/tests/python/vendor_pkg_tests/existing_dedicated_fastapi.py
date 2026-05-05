import fastapi
from fastapi import FastAPI
from workers import WorkerEntrypoint

app = FastAPI()


@app.get("/")
async def root():
    message = "This is an example of FastAPI"
    return {"message": message}


class Default(WorkerEntrypoint):
    async def fetch(self, request):
        import asgi

        return await asgi.fetch(app, request.js_object, self.env)

    async def test(self, a, b, c):
        assert fastapi.__version__
        # The dedicated snapshot we are testing contains source code which is different to
        # below. It will pass the test instead. So we fail this script to show that the snapshot
        # wasn't loaded correctly.
        print("FastAPI dedicated snapshot wasn't loaded")
        raise AssertionError("FastAPI dedicated snapshot wasn't loaded")
