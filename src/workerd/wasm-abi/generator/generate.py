# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Generates the Wasm import modules of the Workers Wasm ABI from JSG RTTI.

Usage:
  python3 -I generate.py (--rtti RTTI.bin | --workerd WORKERD) --compat-date YYYY-MM-DD

For each module in MODULES, writes its record `../modules/<stem>.json` (the machine-readable
import list) and its shim module `../shim/<stem>.js`, where the stem of `cf:http/v1` is
`cf-http-v1`. The RTTI comes from the dump RTTI.bin, or from the workerd binary WORKERD at the
date (see dump_rtti.py). Compat conditions come from `compat-gates.json`, which
compat_gates.py writes.

A module contains the members its include list in MODULES names, and the conventions-defined
imports. The checked-in records are the published form of each module, and released imports are
permanent (see "Versioning" in ../conventions.md): the generator reads the records first, keeps
every import they list in its place and appends the newly included ones. It fails, and writes
nothing, if a released import is no longer included, its member has disappeared or its ABI has
changed, if an included member does not exist or cannot be lowered, or if a member that is not
released yet is deprecated.
"""

import argparse
import json
import re
import sys
import tempfile
import textwrap
from pathlib import Path
from typing import NamedTuple

HERE = Path(__file__).absolute().parent
sys.path.insert(0, str(HERE))

import core_imports
import dump_rtti
import jsfmt
import lowering
import rtti_reader
import typescript

GLOBAL_SCOPE = "workerd::api::ServiceWorkerGlobalScope"
WORKERD_SRC = HERE.parent.parent
RECORDS = HERE.parent / "modules"
SHIMS = HERE.parent / "shim"
GATES = HERE / "compat-gates.json"
GENERATOR = "src/workerd/wasm-abi/generator/generate.py"


class ModuleSpec(NamedTuple):
    """A Wasm import module: its name, the JSG structures whose members are lowered into it, each
    with its include list (the names of the imports to take from it), and its
    conventions-defined imports."""

    name: str
    structures: list
    defined: list


# The modules and their include lists. Members a list does not name are not added yet. To add a
# member, append its import name (see "Import names" in ../conventions.md) to its class's list
# and regenerate: it becomes a new import at the end of the module. A released import can't be
# taken out of its list again. See "Versioning" in ../conventions.md.
MODULES = [
    ModuleSpec("cf:core/v1", [], core_imports.CORE_V1),
    ModuleSpec(
        "cf:http/v1",
        [
            (
                "workerd::api::Headers",
                [
                    "Headers.constructor",
                    "Headers#get",
                    "Headers#has",
                    "Headers#set",
                    "Headers#append",
                    "Headers#delete",
                    "Headers#getSetCookie",
                    "Headers#forEach",
                ],
            ),
            ("workerd::api::Body", ["Body#text", "Body#bytes"]),
            (
                "workerd::api::Request",
                [
                    "Request.constructor",
                    "Request#method.get",
                    "Request#url.get",
                    "Request#headers.get",
                ],
            ),
            (
                "workerd::api::Response",
                [
                    "Response.constructor",
                    "Response#status.get",
                    "Response#statusText.get",
                    "Response#headers.get",
                ],
            ),
            (GLOBAL_SCOPE, ["globalThis.fetch"]),
        ],
        [],
    ),
]

# Members of a module's classes that were already deprecated when they would first have been
# added, and are therefore never added, with the reason; the record lists them under `excluded`.
# A member counts as deprecated if JSG/C++ comments or the TypeScript declarations say so, if a
# compatibility flag at the newest date removes or replaces it, or if it is a legacy non-standard
# extension documented as superseded. The generator refuses to add an included member that is
# listed here or marked `@deprecated` in types/generated-snapshot/index.d.ts. An entry does not
# affect an import that is already released: that import stays, and its record marks it
# deprecated. See "Deprecated members" in ../conventions.md.
DEPRECATED = {
    "Headers#getAll": "Legacy non-standard extension, superseded by Headers#getSetCookie "
    "(comment on Headers::getAll in src/workerd/api/headers.h).",
}

# A module name is `cf:<name>/v<version>`; its shim module is `cf-<name>-v<version>.js`.
MODULE_NAME = re.compile(r"cf:([a-z][a-z0-9]*)/v([1-9][0-9]*)\Z")

# Parameter names that would clash with Rust keywords or with the names the emitters add.
RUST_RESERVED = {
    "type",
    "self",
    "this",
    "ret",
    "fn",
    "move",
    "ref",
    "match",
    "loop",
    "where",
    "impl",
    "mod",
    "use",
    "struct",
    "enum",
    "trait",
    "async",
    "await",
    "dyn",
}


class GenerationError(Exception):
    pass


def stem(module_name):
    """The file name stem of a module's record and shim module: `cf:http/v1` -> `cf-http-v1`."""
    m = MODULE_NAME.match(module_name)
    if m is None:
        raise GenerationError(f"invalid module name {module_name}")
    return f"cf-{m.group(1)}-v{m.group(2)}"


def snake(name):
    s = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name)
    s = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", s)
    return re.sub(r"[^A-Za-z0-9]", "_", s).lower()


def type_summary(t):
    """Renders an RTTI type in C++-ish notation for the record's `origin` field."""
    k = t["kind"]
    if k in ("number", "string", "intrinsic"):
        return t["name"]
    if k == "structure":
        return t["fullyQualifiedName"] or t["name"]
    if k == "promise":
        return "Promise<{}>".format(type_summary(t["value"]))
    if k == "maybe":
        return "{}<{}>".format(t["name"] or "Maybe", type_summary(t["value"]))
    if k == "array":
        return "{}<{}>".format(t["name"], type_summary(t["element"]))
    if k == "dict":
        return "jsg::Dict<{}, {}>".format(
            type_summary(t["key"]), type_summary(t["value"])
        )
    if k == "oneOf":
        return "kj::OneOf<{}>".format(", ".join(type_summary(v) for v in t["variants"]))
    if k == "builtin":
        return t["type"]
    if k == "jsgImpl":
        return "<{}>".format(t["type"])
    if k == "function":
        return "jsg::Function<{}({})>".format(
            type_summary(t["returnType"]),
            ", ".join(type_summary(a) for a in t["args"]),
        )
    if k == "jsBuiltin":
        return "jsBuiltin({}:{})".format(t["module"], t["export"])
    return {"voidt": "void", "boolt": "bool"}.get(k, k)


def annotate(lt):
    """Adds `payload` (the Wasm types of the union payload) to every union L-type."""
    if lt["k"] == "oneof":
        for v in lt["variants"]:
            annotate(v)
        lt["payload"] = lowering.oneof_payload(lt)
    elif lt["k"] == "maybe":
        annotate(lt["inner"])
    return lt


class Member(NamedTuple):
    """A JSG member as one import: the import's kind and names, whether it takes the receiver,
    the RTTI argument types and names, the RTTI return type, and extra fields for the entry."""

    kind: str
    name: str
    binding: str
    member: str
    has_self: bool
    args: list
    arg_names: list
    ret: dict
    extra: dict | None = None


class Lowerer:
    """Lowers the members of JSG structures to import entries. Each entry carries the reason its
    member is deprecated in the TypeScript declarations, if it is, under `_tsDeprecated`."""

    def __init__(self, rtti, ts):
        self.by_name = rtti_reader.structures_by_name(rtti)
        self.exposed = self._exposed_globals()
        self.ts = ts

    def _exposed_globals(self):
        names = set()
        s = self.by_name.get(GLOBAL_SCOPE)
        while s is not None:
            for m in s["members"]:
                if "nested" in m:
                    names.add(m["nested"]["name"] or m["nested"]["structure"]["name"])
            ext = s["extends"]
            s = (
                self.by_name.get(ext["fullyQualifiedName"])
                if ext and ext["kind"] == "structure"
                else None
            )
        return names

    def lower(self, structures):
        """Lowers the included members of the structures. Returns the entries by import name,
        and the import names of all their members."""
        entries, members = {}, set()
        for fqn, include in structures:
            s = self.by_name.get(fqn)
            if s is None:
                raise GenerationError("RTTI lacks structure " + fqn)
            for member in s["members"]:
                for m in self._members(s, member):
                    members.add(m.name)
                    if m.name in include:
                        entries[m.name] = self._entry(s, m)
        return entries, members

    def _ts_class(self, s):
        """The class name the TypeScript declarations use for a structure."""
        if s["fullyQualifiedName"] == GLOBAL_SCOPE:
            return "globalThis"
        return s["tsName"] or s["name"]

    def _js_name(self, s):
        return "globalThis" if s["fullyQualifiedName"] == GLOBAL_SCOPE else s["name"]

    def _members(self, s, member):
        """The imports of one JSG member: one per method or constructor, and a getter and (unless
        read-only) a setter per property."""
        ((kind, m),) = member.items()
        if kind in ("nested", "constant"):
            return []
        js = self._js_name(s)
        if kind == "constructor":
            ret = {
                "kind": "structure",
                "name": s["name"],
                "fullyQualifiedName": s["fullyQualifiedName"],
            }
            ctor = (f"{js}.constructor", f"{snake(js)}_new", "constructor", False)
            return [Member("ctor", *ctor, m["args"], m["argNames"], ret)]
        name = m["name"]
        if kind == "method":
            sig = (m["args"], m["argNames"], m["returnType"])
            if js == "globalThis":
                names = (f"globalThis.{name}", "global_" + snake(name), name, False)
                return [Member("global", *names, *sig)]
            if m["static"]:
                names = (
                    f"{js}.{name}",
                    f"{snake(js)}_static_{snake(name)}",
                    name,
                    False,
                )
                return [Member("static", *names, *sig)]
            names = (f"{js}#{name}", f"{snake(js)}_{snake(name)}", name, True)
            return [Member("method", *names, *sig)]
        extra = {"proto": m["prototype"]}
        getter = (f"{js}#{name}.get", f"{snake(js)}_get_{snake(name)}", name, True)
        out = [Member("get", *getter, [], [], m["type"], extra)]
        if not m["readonly"]:
            setter = (f"{js}#{name}.set", f"{snake(js)}_set_{snake(name)}", name, True)
            void = {"kind": "voidt"}
            out.append(Member("set", *setter, [m["type"]], ["value"], void, extra))
        return out

    def _entry(self, s, m):
        ts_class = self._ts_class(s)
        try:
            params = []
            for idx, a in enumerate(m.args):
                lt = lowering.lower_param(a, self.exposed)
                if lt is None:
                    continue
                pname = m.arg_names[idx] if idx < len(m.arg_names) else None
                params.append(
                    {
                        "name": snake(pname) if pname else None,
                        "lowered": annotate(lt),
                        "rtti": type_summary(a),
                    }
                )
            ts = self.ts.param_names(ts_class, m.member, len(params))
            for i, p in enumerate(params):
                if p["name"] is None:
                    p["name"] = snake(ts[i]) if ts else f"arg{i}"
                if p["name"] in RUST_RESERVED:
                    p["name"] += "_"
            result, suspending = lowering.lower_result(m.ret, self.exposed)
            if m.kind == "ctor":
                result, suspending = (
                    {"k": "handle", "js": s["name"], "of": "structure:" + s["name"]},
                    False,
                )
            annotate(result)
        except lowering.LoweringError as e:
            raise GenerationError(f"{m.name} cannot be lowered: {e}") from None
        entry = {
            "name": m.name,
            "binding": m.binding,
            "kind": m.kind,
            "origin": {
                "structure": s["fullyQualifiedName"],
                "jsName": self._js_name(s),
                "member": m.member,
                "signature": "{}({}) -> {}".format(
                    m.member,
                    ", ".join(type_summary(a) for a in m.args),
                    type_summary(m.ret),
                ),
            },
            "self": m.has_self,
            "params": params,
            "result": result,
            "suspending": suspending,
            "ret": "rc",
        }
        if m.extra:
            entry.update(m.extra)
        entry["_tsDeprecated"] = self.ts.deprecation(ts_class, m.member)
        return entry


def defined_entry(g):
    return {
        "name": g["name"],
        "binding": g["name"],
        "kind": "defined",
        "origin": {"conventions": g["doc"]},
        "self": False,
        "params": [{"name": n, "lowered": lt} for n, lt in g["params"]],
        "result": g["result"],
        "suspending": g["suspending"],
        "ret": g["ret"],
        "initSafe": g["init_safe"],
    }


# ---- Wasm signatures and ABI shapes. ----


def wasm_signature(imp):
    params = []
    if imp["self"]:
        params.append("i32")
    for p in imp["params"]:
        params += lowering.wasm_params(p["lowered"])
    results = []
    if imp["ret"] == "rc":
        if imp["result"]["k"] != "void":
            params.append("i32")
        results = ["i32"]
    elif imp["ret"] == "direct":
        results = lowering.wasm_params(imp["result"])
    return {"params": params, "results": results}


def _shape(lt, in_union=False):
    """The parts of an L-type a guest depends on: kinds, number flags, union variants and their
    order, and the classes a union result tells apart. String kinds, `absent` and handle origins
    outside unions only affect what the API sees, not the guest."""
    k = lt["k"]
    out = {"k": k}
    for flag in ("unsigned", "bigint", "date"):
        if lt.get(flag):
            out[flag] = True
    if k == "handle" and in_union:
        out["js"] = lt.get("js") or lt.get("of")
    if k == "maybe":
        out["inner"] = _shape(lt["inner"], in_union)
    if k == "oneof":
        out["variants"] = [_shape(v, True) for v in lt["variants"]]
    return out


def abi_shape(imp):
    """What a released import must keep: its Wasm signature, whether it suspends, and the shape
    of its lowered parameter and result types."""
    return {
        "wasm": wasm_signature(imp),
        "suspending": imp["suspending"],
        "params": [_shape(p["lowered"]) for p in imp["params"]],
        "result": _shape(imp["result"]),
    }


# ---- Building a module. ----


def compat_conditions(module, name, gates):
    """Compat conditions of a lowered import, from the attribution compat_gates.py recorded."""
    if gates is None:
        return {}
    g = gates["gates"].get(module, {}).get(name)
    if g:
        return {"enableFlag": g["flag"], "enableDate": g["enableDate"]}
    return {
        "enableFlag": None,
        "checkedFrom": gates["oldDate"],
        "checkedTo": gates["newDate"],
    }


def finish(entry, module, gates):
    """Adds the derived fields to an import entry: Wasm signature, result slot, readable types and
    compat conditions."""
    e = dict(entry)
    e["params"] = [
        dict(p, type=lowering.describe(p["lowered"])) for p in entry["params"]
    ]
    e["resultType"] = lowering.describe(entry["result"])
    e["resultSlot"] = lowering.slot(entry["result"]) if entry["ret"] == "rc" else None
    e["wasm"] = wasm_signature(entry)
    if entry["kind"] != "defined":
        e["compat"] = compat_conditions(module, entry["name"], gates)
    return e


class Module(NamedTuple):
    spec: ModuleSpec
    imports: list
    excluded: list
    members: set


def build_module(spec, lowerer, record, gates, complete=True):
    """Builds a module from its include lists and its published record (or None). Released
    imports keep their place and newly included ones are appended in include-list order. With
    `complete` false (an older compatibility date, where flag-gated members are missing),
    included members and released imports may be absent, and released ones are not compared."""
    lowered, members = lowerer.lower(spec.structures)
    included = [n for _, include in spec.structures for n in include]
    candidates = [lowered[n] for n in included if n in lowered]
    candidates += [defined_entry(g) for g in spec.defined]
    released = {i["name"]: i for i in record["imports"]} if record else {}
    errors, built = [], {}
    if complete:
        errors += [
            f"{spec.name}: the include list names {n}, which is not a member of its class in "
            "the RTTI (see conventions.md, Import names)"
            for n in included
            if n not in members
        ]
    for entry in candidates:
        name = entry["name"]
        ts_reason = entry.pop("_tsDeprecated", None)
        reason = DEPRECATED.get(name) or ts_reason
        if name in released:
            if reason:
                entry["deprecated"] = reason
        elif reason:
            listing = (
                ""
                if name in DEPRECATED
                else ", and list it in DEPRECATED with the reason"
            )
            errors.append(
                f"{spec.name}: {name} is deprecated ({reason}) and not released yet, so it must "
                f"not be added: remove it from the include list in generator/generate.py{listing} "
                "(see conventions.md, Deprecated members)"
            )
            continue
        built[name] = finish(entry, spec.name, gates)
    if complete:
        errors += released_errors(spec, released, built, set(included))
    if errors:
        raise GenerationError("\n".join(errors))
    order = [n for n in released if n in built] + [
        n for n in built if n not in released
    ]
    excluded = [
        {"name": n, "reason": r}
        for n, r in DEPRECATED.items()
        if n in members and n not in built
    ]
    return Module(spec, [built[n] for n in order], excluded, members)


def released_errors(spec, released, built, included):
    """Why the built imports break the released ones, if they do."""
    errors = []
    for name, old in released.items():
        if old["kind"] != "defined" and name not in included:
            errors.append(
                f"{spec.name}: the released import {name} is no longer in the include list. A "
                "released import can't be removed: add it back (see conventions.md, Versioning)"
            )
        elif name not in built:
            source = "a member in the RTTI"
            if old["kind"] == "defined":
                source = "a definition in generator/core_imports.py"
            errors.append(
                f"{spec.name}: the released import {name} no longer has {source}. A released "
                "import can't be removed: keep it, or publish a new version of the module "
                "(see conventions.md, Versioning)"
            )
        elif abi_shape(built[name]) != abi_shape(old):
            errors.append(
                f"{spec.name}: the ABI of the released import {name} changed from "
                f"{json.dumps(abi_shape(old))} to {json.dumps(abi_shape(built[name]))}. "
                "A released import can't change: keep the member's signature, or publish "
                "a new version of the module (see conventions.md, Versioning)"
            )
    return errors


def record_json(module, compat_date):
    out = {"module": module.spec.name}
    if module.spec.structures:
        out["compatDate"] = compat_date
    out["generator"] = GENERATOR
    if module.spec.name == "cf:core/v1":
        out["guestExports"] = {
            "memory": "memory with a declared maximum",
            "handle": {"params": ["i32", "i32", "i32"], "results": ["i32"]},
            "init": {"params": [], "results": [], "optional": True},
            "cf_callback": {
                "params": ["i32", "i32"],
                "results": ["i32"],
                "optional": "required if any callback token is passed",
            },
        }
    out["imports"] = module.imports
    if module.spec.structures:
        out["excluded"] = module.excluded
    return jsfmt.json_text(out)


COPYRIGHT = """\
// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
"""


def shim_descriptor(imp):
    """What runtime.js needs to implement an import."""
    if imp["kind"] == "defined":
        return {
            "n": imp["name"],
            "t": "defined",
            "p": [p["lowered"] for p in imp["params"]],
            "r": imp["result"],
            "ret": imp["ret"],
            "s": imp["suspending"],
            "init": imp["initSafe"],
        }
    d = {
        "n": imp["name"],
        "t": imp["kind"],
        "js": imp["origin"]["jsName"],
        "m": imp["origin"]["member"],
        "self": imp["self"],
        "p": [p["lowered"] for p in imp["params"]],
        "r": imp["result"],
        "s": imp["suspending"],
    }
    if "proto" in imp:
        d["proto"] = imp["proto"]
    return d


def shim_js(module, compat_date):
    source = ""
    if module.spec.structures:
        source = f" from JSG RTTI at compatibility date {compat_date}"
    about = textwrap.fill(
        f"The shim module of the Wasm import module {module.spec.name}, which runtime.js "
        f"serves. Generated by {GENERATOR}{source}; do not edit. See conventions.md "
        "(Tooling).",
        width=80,
        initial_indent="// ",
        subsequent_indent="// ",
    )
    body = {
        "module": module.spec.name,
        "imports": [shim_descriptor(i) for i in module.imports],
    }
    return f"{COPYRIGHT}\n{about}\n\n" + jsfmt.js_default_export(body)


# ---- Compatibility flags. ----


def parse_compat_flags():
    """Maps each compatibility flag name to its enable date, from compatibility-date.capnp."""
    text = (WORKERD_SRC / "io" / "compatibility-date.capnp").read_text()
    out = []
    for m in re.finditer(r"(\w+)\s+@\d+\s*:Bool(.*?);", text, re.DOTALL):
        body = m.group(2)
        flag = re.search(r'\$compatEnableFlag\("([^"]+)"\)', body)
        date = re.search(r'\$compatEnableDate\("([^"]+)"\)', body)
        out.append(
            {
                "field": m.group(1),
                "flag": flag.group(1) if flag else None,
                "enableDate": date.group(1) if date else None,
            }
        )
    return out


def load_gates(path=GATES):
    path = Path(path)
    return json.loads(path.read_text()) if path.exists() else None


def load_record(spec):
    path = RECORDS / (stem(spec.name) + ".json")
    return json.loads(path.read_text()) if path.exists() else None


class Output(NamedTuple):
    """The generated files of one module."""

    module: Module
    record: str
    shim: str


def generate(rtti, compat_date, ts=None, gates=None, complete=True):
    """Generates every module from parsed RTTI. Returns {module name: Output}."""
    ts = ts if ts is not None else typescript.Declarations.load()
    lowerer = Lowerer(rtti, ts)
    out = {}
    for spec in MODULES:
        module = build_module(spec, lowerer, load_record(spec), gates, complete)
        out[spec.name] = Output(
            module, record_json(module, compat_date), shim_js(module, compat_date)
        )
    return out


def unused_exclusions(outputs):
    """Entries of DEPRECATED that match no member of a module's classes."""
    members = set().union(*(o.module.members for o in outputs.values()))
    return sorted(set(DEPRECATED) - members)


def read_rtti(args_rtti, workerd, date):
    """Parsed RTTI from a dump, or dumped with a workerd binary at `date`."""
    if args_rtti is not None:
        return rtti_reader.read_rtti(args_rtti)
    with (
        dump_rtti.rtti_server(workerd) as get,
        tempfile.NamedTemporaryFile(suffix=".capnp.bin") as f,
    ):
        f.write(get(date))
        f.flush()
        return rtti_reader.read_rtti(f.name)


def main():
    ap = argparse.ArgumentParser()
    source = ap.add_mutually_exclusive_group(required=True)
    source.add_argument("--rtti", help="RTTI dump at --compat-date")
    source.add_argument("--workerd", help="workerd binary to dump the RTTI with")
    ap.add_argument("--compat-date", required=True)
    args = ap.parse_args()

    rtti = read_rtti(args.rtti, args.workerd, args.compat_date)
    try:
        outputs = generate(rtti, args.compat_date, gates=load_gates())
    except GenerationError as e:
        raise SystemExit(f"error: {e}") from None
    for name, o in outputs.items():
        (RECORDS / (stem(name) + ".json")).write_text(o.record)
        (SHIMS / (stem(name) + ".js")).write_text(o.shim)
        m = o.module
        print(f"{name}: {len(m.imports)} imports, {len(m.excluded)} excluded")
    for name in unused_exclusions(outputs):
        print(f"warning: DEPRECATED lists {name}, which matches no member")


if __name__ == "__main__":
    main()
