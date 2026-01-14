import signal
import sys
from inspect import isawaitable, isclass
from types import FunctionType

import js
from pyodide.code import relaxed_call
from pyodide.ffi import to_js
from workers import (
    DurableObject,
    WorkerEntrypoint,
    WorkflowEntrypoint,
    python_from_rpc,
    python_to_rpc,
)


def getattr_no_get(cls, name):
    """Get attribute from class, don't run __get__"""
    for base in cls.__mro__:
        if name in base.__dict__:
            return base.__dict__[name]
    return None


def collect_methods(cls):
    """
    Iterates through the methods in `cls` and returns only public non-static/non-class methods
    defined on that class.
    """
    return sorted(
        name
        for name in dir(cls)
        if not name.startswith("_")
        and isinstance(getattr_no_get(cls, name), FunctionType)
    )


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


async def wrapper_func(relaxed, inst, prop, *args, **kwargs):
    method = getattr(inst, prop)

    py_args = [python_from_rpc(arg) for arg in args]
    py_kwargs = {k: python_from_rpc(v) for k, v in kwargs.items()}
    result = (
        relaxed_call(method, *py_args, **py_kwargs)
        if relaxed
        else method(*py_args, **py_kwargs)
    )

    if isawaitable(result):
        return python_to_rpc(await result)
    else:
        return python_to_rpc(result)


class CpuLimitExceeded(BaseException):
    pass


def raise_cpu_limit_exceeded(signum, frame):
    raise CpuLimitExceeded("Python Worker exceeded CPU time limit")


if sys.platform == "emscripten":
    signal.signal(signal.SIGXCPU, raise_cpu_limit_exceeded)
