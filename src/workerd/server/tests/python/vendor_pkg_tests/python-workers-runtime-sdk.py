from workers import extra_function


async def test():
    assert extra_function() == 42
    print("workers-sdk imported successfully!")
