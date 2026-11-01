# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Lowers JSG RTTI types to Wasm signatures, following `../conventions.md`.

A lowered type ("L-type") is a small dictionary with a `k` field:

  void                       no value
  bool                       i32, 0 or 1
  i32  {unsigned}            i32
  i64  {bigint}              i64; `bigint` means the JS side is a BigInt, otherwise a Number
  f64  {date}                f64; `date` means milliseconds since the epoch, a JS Date
  string {kind}              UTF-8 bytes; `kind` is the JSG string type name
  bytes {js}                 raw bytes; `js` is "Uint8Array" or "ArrayBuffer"
  handle {js, of}            a handle-table entry; `js` names the global constructor of a JSG
                             resource when there is one, `of` describes the RTTI origin
  callback {arity}           a guest callback token (parameters only)
  maybe {inner, absent}      optional value; `absent` is "null" or "undefined"
  oneof {variants}           tagged union, tags are variant indices
  varargs                    a handle to a JS array spread into the call (parameters only)

The `promise` RTTI kind never appears as an L-type: a top-level promise result makes the import
suspending and lowers to the awaited value.
"""

HANDLE_LIKE = ("handle", "callback")

# C++ number type names from `FOR_EACH_NUMBER_TYPE` in `jsg/rtti.h`.
NUMBERS = {
    "char": ("i32", False),
    "signed char": ("i32", False),
    "unsigned char": ("i32", True),
    "short": ("i32", False),
    "unsigned short": ("i32", True),
    "int": ("i32", False),
    "unsigned int": ("i32", True),
    "jsg::JsInt32": ("i32", False),
    "jsg::JsUint32": ("i32", True),
    "long": ("i64", False),
    "unsigned long": ("i64", False),
    "long long": ("i64", False),
    "unsigned long long": ("i64", False),
    "jsg::JsBigInt": ("i64", True),
    "double": ("f64", False),
    "jsg::JsNumber": ("f64", False),
}

BYTE_ELEMENTS = ("unsigned char", "char", "signed char")

SKIPPED_IMPL = {
    "configuration",
    "v8Isolate",
    "jsgLock",
    "jsgTypeHandler",
    "jsgSelfRef",
    "v8FunctionCallbackInfo",
    "v8PropertyCallbackInfo",
    "jsgUnimplemented",
}


class LoweringError(Exception):
    pass


def js_name_of(structure_type, exposed):
    """The global constructor name for a structure type, or None if it is not a global."""
    name = structure_type.get("name", "")
    return name if name in exposed else None


def lower(t, pos, exposed):
    """Lowers an RTTI type. `pos` is "param" or "result". Returns (ltype, suspending)."""
    k = t["kind"]
    if k == "promise":
        if pos == "result":
            inner, _ = lower(t["value"], pos, exposed)
            return inner, True
        return {"k": "handle", "js": "Promise", "of": "promise"}, False
    return _lower_value(t, pos, exposed), False


def _lower_value(t, pos, exposed):
    k = t["kind"]
    if k == "voidt":
        return {"k": "void"}
    if k == "boolt":
        return {"k": "bool"}
    if k == "number":
        name = t["name"]
        if name not in NUMBERS:
            raise LoweringError("unknown number type " + name)
        wasm, flag = NUMBERS[name]
        if wasm == "i32":
            return {"k": "i32", "unsigned": flag}
        if wasm == "i64":
            return {"k": "i64", "bigint": flag}
        return {"k": "f64", "date": False}
    if k == "string":
        return {"k": "string", "kind": t["name"]}
    if k == "structure":
        return {
            "k": "handle",
            "js": js_name_of(t, exposed),
            "of": "structure:" + t["name"],
        }
    if k in ("unknown", "object", "dict", "intrinsic", "jsBuiltin"):
        return {"k": "handle", "js": None, "of": k}
    if k == "array":
        el = t["element"]
        if el["kind"] == "number" and el["name"] in BYTE_ELEMENTS:
            return {"k": "bytes", "js": "ArrayBuffer"}
        return {"k": "handle", "js": "Array", "of": "array:" + t["name"]}
    if k == "builtin":
        b = t["type"]
        if b in ("v8Uint8Array", "v8ArrayBufferView", "jsgBufferSource"):
            return {"k": "bytes", "js": "Uint8Array"}
        if b == "v8ArrayBuffer":
            return {"k": "bytes", "js": "ArrayBuffer"}
        if b == "kjDate":
            return {"k": "f64", "date": True}
        return {"k": "handle", "js": None, "of": "builtin:" + b}
    if k == "function":
        if pos == "param":
            return {"k": "callback", "arity": len(t["args"])}
        return {"k": "handle", "js": "Function", "of": "function"}
    if k == "jsgImpl":
        if t["type"] == "jsgVarargs":
            return {"k": "varargs"}
        if t["type"] == "jsgName":
            return {"k": "handle", "js": None, "of": "jsgName"}
        raise LoweringError("jsgImpl {} is not a JS-visible value".format(t["type"]))
    if k == "maybe":
        return _lower_maybe(t, pos, exposed)
    if k == "oneOf":
        return _lower_oneof(t, pos, exposed)
    if k == "promise":
        return {"k": "handle", "js": "Promise", "of": "promise"}
    raise LoweringError("cannot lower RTTI kind " + k)


def _absent_of(t):
    if t.get("allowsUndefined"):
        return "undefined"
    if t.get("allowsNull"):
        return "null"
    # Legacy producers set neither flag; the name says which alternative it is.
    return "null" if t.get("name") == "kj::Maybe" else "undefined"


def _lower_maybe(t, pos, exposed):
    absent = _absent_of(t)
    inner = _lower_value(t["value"], pos, exposed)
    if inner["k"] == "maybe":
        # Optional<Maybe<T>>: one level of optionality; the outer type decides what absent means.
        inner = inner["inner"]
    return {"k": "maybe", "inner": inner, "absent": absent}


def _lower_oneof(t, pos, exposed):
    variants = []
    absent = None
    for v in t["variants"]:
        lv = _lower_value(v, pos, exposed)
        if lv["k"] == "oneof":
            parts = lv["variants"]
        elif lv["k"] == "maybe":
            absent = absent or lv["absent"]
            inner = lv["inner"]
            parts = inner["variants"] if inner["k"] == "oneof" else [inner]
        else:
            parts = [lv]
        for p in parts:
            if p not in variants:
                variants.append(p)
    result = {"k": "oneof", "variants": variants}
    if pos == "param" and all(v["k"] == "handle" for v in variants):
        # JSG discriminates JS objects itself, so a union of handles needs no tag.
        result = {"k": "handle", "js": None, "of": "oneof"}
    if absent:
        return {"k": "maybe", "inner": result, "absent": absent}
    return result


def lower_result(t, exposed):
    """Lowers a return type. A union of promises becomes a promise of a union."""
    if (
        t["kind"] == "oneOf"
        and t["variants"]
        and all(v["kind"] == "promise" for v in t["variants"])
    ):
        merged = {"kind": "oneOf", "variants": [v["value"] for v in t["variants"]]}
        lt = _lower_value(merged, "result", exposed)
        if lt["k"] == "oneof" and len(lt["variants"]) == 1:
            lt = lt["variants"][0]
        return lt, True
    lt, suspending = lower(t, "result", exposed)
    if lt["k"] == "maybe" and lt["inner"]["k"] == "void":
        lt = {"k": "void"}
    return lt, suspending


def lower_param(t, exposed):
    """Lowers a parameter type, or returns None for JSG implementation parameters that JS
    never sees (the lock, the type handler and so on)."""
    if t["kind"] == "jsgImpl" and t["type"] in SKIPPED_IMPL:
        return None
    lt, _ = lower(t, "param", exposed)
    return lt


# ---- Wasm signatures. ----


def wasm_params(lt):
    """The Wasm value types a parameter of L-type `lt` occupies, in order."""
    k = lt["k"]
    if k in ("bool", "handle", "callback", "varargs"):
        return ["i32"]
    if k == "i32":
        return ["i32"]
    if k == "i64":
        return ["i64"]
    if k == "f64":
        return ["f64"]
    if k in ("string", "bytes"):
        return ["i32", "i32"]
    if k == "maybe":
        if lt["inner"]["k"] in HANDLE_LIKE:
            return ["i32"]
        return ["i32", *wasm_params(lt["inner"])]
    if k == "oneof":
        return ["i32", *oneof_payload(lt)]
    raise LoweringError("no parameter lowering for " + k)


def oneof_payload(lt):
    """Payload slots of a union parameter: position k is i32 if every variant's k-th slot is an
    i32 (or missing), otherwise i64. A variant's f64 travels as its IEEE-754 bits in an i64."""
    lists = [wasm_params(v) for v in lt["variants"]]
    width = max(len(params) for params in lists)
    out = []
    for i in range(width):
        types = {params[i] for params in lists if i < len(params)}
        out.append("i32" if types <= {"i32"} else "i64")
    return out


# ---- Result slots in guest memory. ----

STR_SLOT = {"size": 16, "align": 4}


def slot(lt):
    """Size and alignment of the memory slot a result of L-type `lt` is written to."""
    k = lt["k"]
    if k in ("bool", "i32", "handle", "callback"):
        return {"size": 4, "align": 4}
    if k in ("i64", "f64"):
        return {"size": 8, "align": 8}
    if k in ("string", "bytes"):
        return dict(STR_SLOT)
    if k == "maybe":
        if lt["inner"]["k"] in HANDLE_LIKE:
            return {"size": 4, "align": 4}
        inner = slot(lt["inner"])
        return {"size": 8 + _round8(inner["size"]), "align": 8}
    if k == "oneof":
        size = max(slot(v)["size"] for v in lt["variants"])
        return {"size": 8 + _round8(size), "align": 8}
    if k == "void":
        return {"size": 0, "align": 1}
    raise LoweringError("no result slot for " + k)


def _round8(n):
    return (n + 7) & ~7


def describe(lt):
    """A short human-readable rendering of an L-type, used in docs and the JSON."""
    k = lt["k"]
    if k == "i32":
        return "u32" if lt.get("unsigned") else "i32"
    if k == "i64":
        return "bigint" if lt.get("bigint") else "i64"
    if k == "f64":
        return "date" if lt.get("date") else "f64"
    if k == "string":
        return "string({})".format(lt["kind"])
    if k == "bytes":
        return "bytes({})".format(lt["js"])
    if k == "handle":
        return f"handle({lt['js'] or lt['of']})"
    if k == "callback":
        return f"callback/{lt['arity']}"
    if k == "maybe":
        return "maybe<{}>".format(describe(lt["inner"]))
    if k == "oneof":
        return "oneof<{}>".format(", ".join(describe(v) for v in lt["variants"]))
    return k
