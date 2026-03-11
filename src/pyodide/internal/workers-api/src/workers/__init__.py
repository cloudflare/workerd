from ._workers import (
    Blob,
    BlobEnding,
    BlobValue,
    Body,
    Context,
    DurableObject,
    FetchKwargs,
    FetchResponse,
    File,
    FormData,
    FormDataValue,
    Headers,
    JSBody,
    Request,
    RequestInitCfProperties,
    Response,
    WorkerEntrypoint,
    WorkflowEntrypoint,
    fetch,
    handler,
    import_from_javascript,
    patch_env,
    python_from_rpc,
    python_to_rpc,
)

__all__ = [
    "Blob",
    "BlobEnding",
    "BlobValue",
    "Body",
    "Context",
    "DurableObject",
    "FetchKwargs",
    "FetchResponse",
    "File",
    "FormData",
    "FormDataValue",
    "Headers",
    "JSBody",
    "Request",
    "RequestInitCfProperties",
    "Response",
    "WorkerEntrypoint",
    "WorkflowEntrypoint",
    "env",
    "fetch",
    "handler",
    "import_from_javascript",
    "patch_env",
    "python_from_rpc",
    "python_to_rpc",
    "waitUntil",
]


def __getattr__(key):
    if key == "env":
        cloudflare_workers = import_from_javascript("cloudflare:workers")
        return cloudflare_workers.env
    if key == "waitUntil":
        cloudflare_workers = import_from_javascript("cloudflare:workers")
        return cloudflare_workers.waitUntil
    raise AttributeError(f"module {__name__!r} has no attribute {key!r}")
