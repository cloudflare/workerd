from inspect import isclass

from workers import DurableObject

from pyodide.ffi import to_js


def collect_classes(user_mod):
    """
    Iterates through the defined symbols in the input module. Returns any classes which extend
    DurableObject.
    """
    if hasattr(user_mod, "__all__"):
        keys = user_mod.__all__
    else:
        keys = (key for key in dir(user_mod) if not key.startswith("_"))

    def filter(key):
        val = getattr(user_mod, key)
        return (
            isclass(val) and issubclass(val, DurableObject) and val is not DurableObject
        )

    return to_js([key for key in keys if filter(key)])
