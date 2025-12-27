import fastapi
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    async def test(self):
        assert fastapi.__version__
        print("FastAPI imported successfully!")
