import shapely
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    async def test(self):
        assert shapely.__version__
        print("shapely imported successfully!")
