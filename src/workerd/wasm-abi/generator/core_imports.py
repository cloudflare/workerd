# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""The imports of `cf:core/v1`, which the conventions define rather than lowering them from a JSG
member.

Each entry uses the same shape as a lowered import: `params` is a list of (name, L-type) pairs,
`result` is an L-type, and `ret` says how the import returns:

  "rc"      returns an i32 status: 0 on success, otherwise the handle of the thrown value. A
            non-void result is written to the slot passed as the last parameter.
  "direct"  returns its result as the Wasm return value and cannot throw.
  "none"    returns nothing.

`../conventions.md` documents each of them; the shim implements them in `../shim/runtime.js`.
Released entries are permanent (see "Versioning" in the conventions): add new imports, never
remove or change one.
"""

H = {"k": "handle", "js": None, "of": "unknown"}
MAYBE_H = {"k": "maybe", "inner": H, "absent": "undefined"}
STR = {"k": "string", "kind": "DOMString"}
BYTES = {"k": "bytes", "js": "Uint8Array"}
U32 = {"k": "i32", "unsigned": True}
F64 = {"k": "f64", "date": False}
BOOL = {"k": "bool"}
VOID = {"k": "void"}


# The options of an entry: how it returns (see above), whether it suspends, whether `init` may
# call it, and its documentation.
OPTIONS = {
    "ret": "rc",
    "suspending": False,
    "init_safe": False,
    "doc": "",
}


def g(name, params, result=VOID, **options):
    unknown = options.keys() - OPTIONS.keys()
    if unknown:
        raise TypeError(f"{name}: unknown options {sorted(unknown)}")
    o = OPTIONS | options
    return {
        "name": name,
        "params": params,
        "result": result,
        "ret": o["ret"],
        "suspending": o["suspending"],
        "init_safe": o["init_safe"],
        "doc": o["doc"],
    }


CORE_V1 = [
    # Handle table.
    g(
        "handle_drop",
        [("h", H)],
        ret="none",
        doc="Releases a handle. Traps on an invalid handle.",
    ),
    # Values.
    g("value_undefined", [], H, ret="direct", doc="New handle holding `undefined`."),
    g("value_null", [], H, ret="direct", doc="New handle holding `null`."),
    g(
        "value_bool",
        [("v", BOOL)],
        H,
        ret="direct",
        doc="New handle holding a boolean.",
    ),
    g("value_f64", [("v", F64)], H, ret="direct", doc="New handle holding a number."),
    g(
        "value_string",
        [("s", STR)],
        H,
        ret="direct",
        doc="New handle holding a string.",
    ),
    g(
        "value_bytes",
        [("b", BYTES)],
        H,
        ret="direct",
        doc="New handle holding a Uint8Array copy.",
    ),
    g(
        "value_kind",
        [("h", H)],
        U32,
        ret="direct",
        doc="Kind code of a handle's value; see the value kinds table.",
    ),
    g("value_to_bool", [("h", H)], BOOL, ret="direct", doc="ToBoolean of the value."),
    g("value_to_f64", [("h", H)], F64, doc="ToNumber of the value."),
    g("value_to_string", [("h", H)], STR, doc="ToString of the value, as UTF-8."),
    g("value_to_bytes", [("h", H)], BYTES, doc="Bytes of an ArrayBuffer or view."),
    # Objects.
    g("object_new", [], H, ret="direct", doc="New handle holding `{}`."),
    g(
        "object_set",
        [("obj", H), ("key", STR), ("v", H)],
        doc="obj[key] = value of handle v.",
    ),
    g(
        "object_set_string",
        [("obj", H), ("key", STR), ("v", STR)],
        doc="obj[key] = string.",
    ),
    g(
        "object_set_f64",
        [("obj", H), ("key", STR), ("v", F64)],
        doc="obj[key] = number.",
    ),
    g(
        "object_set_bool",
        [("obj", H), ("key", STR), ("v", BOOL)],
        doc="obj[key] = boolean.",
    ),
    # Bindings.
    g(
        "env_get",
        [("env", H), ("name", STR)],
        MAYBE_H,
        doc="The binding `name` of the handler's env (a text variable, secret or binding object);"
        " handle 0 if there is none.",
    ),
    # Arrays.
    g("array_new", [], H, ret="direct", doc="New handle holding `[]`."),
    g("array_push", [("arr", H), ("v", H)], doc="arr.push(value of handle v)."),
    g(
        "array_len",
        [("arr", H)],
        U32,
        ret="direct",
        doc="arr.length. Traps if not an array.",
    ),
    g("array_get", [("arr", H), ("i", U32)], H, doc="New handle holding arr[i]."),
    # Errors.
    g(
        "error_new",
        [("name", STR), ("message", STR)],
        H,
        ret="direct",
        doc="New Error with the given name and message.",
    ),
    g(
        "error_name",
        [("err", H)],
        STR,
        doc="The error's name, or empty for a non-Error value.",
    ),
    g(
        "error_message",
        [("err", H)],
        STR,
        doc="The error's message, or ToString of a non-Error value.",
    ),
    # Request lifecycle.
    g(
        "respond",
        [("response", H)],
        doc="Sends the response now; the handler keeps running.",
    ),
    # Console.
    g(
        "console_log",
        [("level", U32), ("msg", STR)],
        ret="none",
        doc="console.debug/log/warn/error for level 0/1/2/3.",
    ),
    g(
        "panic",
        [("msg", STR)],
        ret="none",
        init_safe=True,
        doc="Logs the message with console.error and traps.",
    ),
]
