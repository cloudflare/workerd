import pywt
import pywt.data
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    async def test(self):
        assert pywt.__version__
        print("PyWavelets imported successfully!")
