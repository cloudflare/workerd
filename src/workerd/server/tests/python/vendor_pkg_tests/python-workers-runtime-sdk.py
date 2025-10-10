# The python-workers-runtime-sdk vendored files include the sdk in tree from workerd
# 9ca3b8e36e86e34f16ecd9680334ba0e0261549c plus one extra function. If we don't at least have the
# things imported in introspection.py we'll crash on startup.
from workers import extra_function


async def test():
    assert extra_function() == 42
    print("workers-sdk imported successfully!")
