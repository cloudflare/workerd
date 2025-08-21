"""
A metapath finder which calls get_import_context(module_name). If it returns a
value that is not None, this is interpreted as a context manager that should be
used when executing the module top level scope.

When we're done, we put back the original module. The wrapper module and wrapper
stubs will persist in the wild, so we need to make sure they behave the same way
as the originals after we put them back. This is controlled by the
IN_REQUEST_CONTEXT variable.
"""

import sys
from collections.abc import Callable
from contextlib import AbstractContextManager, nullcontext
from dataclasses import dataclass
from functools import partial, wraps
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from importlib.abc import Loader
    from importlib.machinery import ModuleSpec
    from types import ModuleType

    CreateImportContext = Callable[[ModuleSpec], AbstractContextManager]
    ExecImportContext = Callable[[ModuleType], AbstractContextManager]
    BeforeFirstRequest = Callable[[ModuleType], None]
else:
    from typing import Any

    BeforeFirstRequest = Any
    CreateImportContext = Any
    ExecImportContext = Any
    Loader = Any
    ModuleSpec = Any
    ModuleType = Any


@dataclass
class PatchInfo:
    create: CreateImportContext = nullcontext
    exec: ExecImportContext = nullcontext
    before_first_request: BeforeFirstRequest | None = None


patches: dict[str, PatchInfo] = {}


# The public-facing interface here is:
# * register_create_patch
# * register_exec_patch
# * register_before_first_request
# * block_calls


def register_create_patch(
    name: str, context_manager: CreateImportContext | None = None
) -> None:
    """This registers a context_manager that will be used around the create_module call when the
    package named "name" is imported.

    It can either be used like register_exec_patch("cryptography.exceptions", rust_package_context)
    or as a decorator.
    """
    if context_manager is None:
        return partial(register_create_patch, name)
    d = patches.setdefault(name, PatchInfo())
    d.create = context_manager
    return context_manager


def register_exec_patch(
    name: str, context_manager: ExecImportContext | None = None
) -> None:
    """This registers a context_manager that will be used around the exec_module call when the
    package named "name" is imported.

    It can either be used like register_create_patch("tiktoken._tiktoken", rust_package_context)
    or as a decorator.
    """
    if context_manager is None:
        return partial(register_exec_patch, name)
    d = patches.setdefault(name, PatchInfo())
    d.exec = context_manager
    return context_manager


# Question: How do I decide whether to use register_create_patch or register_exec_patch?
#
# Answer: For pure Python packages, use register_exec_patch(). For extension modules, figure it out
# by trial and error.


def register_before_first_request(
    name: str, handler: BeforeFirstRequest | None = None
) -> None:
    """This registers a callback that will be called before the first request if the package named
    "name" was imported. Used for reseeding rng for instance.
    """
    if handler is None:
        return partial(register_before_first_request, name)
    d = patches.setdefault(name, PatchInfo())
    d.before_first_request = handler
    return handler


before_first_request_handlers: list[BeforeFirstRequest] = []


class PatchLoader:
    """Loader that calls the original exec_module in the given context manager"""

    def __init__(self, orig_loader: Loader, patch_info: PatchInfo):
        self.orig_loader = orig_loader
        self.patch_info = patch_info

    def __getattr__(self, name):
        return getattr(self.orig_loader, name)

    def create_module(self, spec: ModuleSpec) -> ModuleType | None:
        with self.patch_info.create(spec):
            return self.orig_loader.create_module(spec)

    def exec_module(self, module: ModuleType) -> None:
        if self.patch_info.before_first_request:
            before_first_request_handlers.append(
                partial(self.patch_info.before_first_request, module)
            )
        with self.patch_info.exec(module):
            self.orig_loader.exec_module(module)


class PatchFinder:
    """Finder that returns our PatchLoader if get_import_context returns an import
    context for the module. Otherwise, return None.
    """

    def invalidate_caches(self):
        pass

    def find_spec(
        self,
        fullname: str,
        path,
        target,
    ):
        import_context = patches.get(fullname, None)
        if import_context is None:
            # Not ours
            return None

        for finder in sys.meta_path:
            if isinstance(finder, PatchFinder):
                # Avoid infinite recurse. Presumably this is the first entry.
                continue
            spec = finder.find_spec(fullname, path, target)
            if spec:
                # Found original module spec
                break
        else:
            # Not found. This is going to be an ImportError.
            return None
        # Overwrite the loader with our wrapped loader
        spec.loader = PatchLoader(spec.loader, import_context)
        return spec

    @staticmethod
    def install():
        sys.meta_path.insert(0, PatchFinder())

    @staticmethod
    def remove():
        for idx, val in enumerate(sys.meta_path):  # noqa:B007
            if isinstance(val, PatchFinder):
                break
        del sys.meta_path[idx]


def install_import_patch_manager():
    PatchFinder.install()


def remove_import_patch_manager():
    PatchFinder.remove()
    unblock_calls()


# We remove the metapath entry and replace the patched sys.modules entries with
# the original modules before the request context, but the patched copies can
# still be used from top level imports. When IN_REQUEST_CONTEXT is True, we need
# to make sure that our patches behave like the original imports.
IN_REQUEST_CONTEXT = False
# Keep track of the unblocked modules so we can put them backk into sys.modules
# when we're done.
ORIG_MODULES = {}


def block_calls(module, *, allowlist=()):
    """Make top level calls to methods from the module that are not in allowlist fail.

    It gets removed automatically before the first request. Generally used with
    register_before_first_request.
    """
    sys.modules[module.__name__] = BlockedCallModule(module, allowlist)
    ORIG_MODULES[module.__name__] = module


def unblock_calls():
    # Remove the patches when we're ready to enable entropy calls.
    global IN_REQUEST_CONTEXT

    IN_REQUEST_CONTEXT = True
    for name, val in ORIG_MODULES.items():
        sys.modules[name] = val


class BlockedCallModule:
    """A proxy class that wraps a module that we want to block calls to

    Attribute access is passed on to the original module but if the result is a
    callable that isn't in the allow list, we wrap it with a function that
    raises an error unless IN_REQUEST_CONTEXT is true.

    Note that because we define __getattribute__ and __setattr__, we cannot do
    direct reads or assignments e.g., `self.a = 1`. This risks recursion errors
    if there is a typo. Instead, we have to call super().__setattr__.

    This has the advantage that it avoids name clashes if the proxied module
    actually defines variables called _mod or _allow_list.
    """

    def __init__(self, module, allowlist):
        super().__setattr__("_mod", module)
        super().__setattr__("_allow_list", allowlist)

    def __getattribute__(self, key):
        mod = super().__getattribute__("_mod")
        orig = getattr(mod, key)
        if IN_REQUEST_CONTEXT:
            return orig
        if not callable(orig):
            return orig

        if key in super().__getattribute__("_allow_list"):
            return orig

        # If we aren't in a request scope, the value is a callable, and it's not
        # in the allow_list, return a wrapper that raises an error if it's
        # called before entering the request scope.
        # TODO: this doesn't wrap classes correctly, does it matter?
        @wraps(orig)
        def wrapper(*args, **kwargs):
            if not IN_REQUEST_CONTEXT:
                raise RuntimeError(
                    f"Cannot use {mod.__name__}.{key}() outside of request context"
                )
            return orig(*args, **kwargs)

        return wrapper

    def __setattr__(self, key, val):
        mod = super().__getattribute__("_mod")
        setattr(mod, key, val)

    def __dir__(self):
        mod = super().__getattribute__("_mod")
        return dir(mod)
