from inspect import isclass

import js
from workers import DurableObject, WorkerEntrypoint, WorkflowEntrypoint

from pyodide.ffi import to_js


def collect_methods(class_val):
    """
    Iterates through the methods in `class_val` and returns only public non-static/non-class methods
    defined on that class.
    """
    return [
        name
        for name, value in class_val.__dict__.items()
        if not isinstance(value, (classmethod, staticmethod))
        and not name.startswith("_")
    ]


def collect_classes(user_mod, base_cls):
    """
    Iterates through the defined symbols in the input module. Returns any classes which extend
    `base_cls` (where `base_cls` is one of DurableObject, WorkerEntrypoint or WorkflowEntrypoint).

    This method returns a list of JS objects like [{"className": "MyClass", "methodNames": ["foo"]}].]
    """
    if hasattr(user_mod, "__all__"):
        keys = user_mod.__all__
    else:
        keys = (key for key in dir(user_mod) if not key.startswith("_"))

    exported_attrs = [getattr(user_mod, key) for key in keys]

    def filter(val):
        return isclass(val) and issubclass(val, base_cls) and val is not base_cls

    class_attrs = [attr for attr in exported_attrs if filter(attr)]
    result = [
        {"className": attr.__name__, "methodNames": collect_methods(attr)}
        for attr in class_attrs
    ]
    return to_js(result, dict_converter=js.Object.fromEntries)


def collect_entrypoint_classes(user_mod):
    return to_js(
        {
            "durableObjects": collect_classes(user_mod, DurableObject),
            "workerEntrypoints": collect_classes(user_mod, WorkerEntrypoint),
            "workflowEntrypoints": collect_classes(user_mod, WorkflowEntrypoint),
        },
        dict_converter=js.Object.fromEntries,
    )
