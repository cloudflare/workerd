from inspect import isclass

from workers import DurableObject, WorkerEntrypoint, WorkflowEntrypoint

from pyodide.ffi import to_js


def collect_classes(user_mod, cls):
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
        return isclass(val) and issubclass(val, cls) and val is not cls

    return to_js([key for key in keys if filter(key)])


def collect_durable_object_classes(user_mod):
    return collect_classes(user_mod, DurableObject)


def collect_worker_entrypoint_classes(user_mod):
    return collect_classes(user_mod, WorkerEntrypoint)


def collect_workflow_entrypoint_classes(user_mod):
    return collect_classes(user_mod, WorkflowEntrypoint)
