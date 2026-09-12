from PIL import Image
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    async def test(self):
        assert Image.new("RGB", (10, 10))
        print("Pillow imported successfully!")
