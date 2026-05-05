import fastapi
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    def test(self):
        assert fastapi.__version__ in {"0.110.0", "0.116.1"}
