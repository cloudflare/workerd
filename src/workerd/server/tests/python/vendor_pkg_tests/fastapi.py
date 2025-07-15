import fastapi


async def test():
    assert fastapi.__version__
    print("FastAPI imported successfully!")
