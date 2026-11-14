# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Self-test of the Wasm import modules.

Usage:
  python3 -I self_test.py --workerd WORKERD --compat-date D1 --old-date D0 [-v]
  python3 -I self_test.py --rtti NEW.bin --compat-date D1 --old-rtti OLD.bin --old-date D0 [-v]

With --workerd, the RTTI for both dates is dumped with that binary (see dump_rtti.py); otherwise
it is read from the given dumps. `bazel test //src/workerd/wasm-abi:generator-self-test` runs the
first form at the dates the checked-in modules were generated for.

Checks, for every module in generate.MODULES:
  1. Its checked-in record (../modules/<stem>.json) and shim module (../shim/<stem>.js) are what
     the generator produces from the RTTI at D1. This also fails if the generator refuses: a
     released import was removed or changed, an included member is missing or cannot be
     lowered, or a deprecated member is about to be added.
  2. Every member that the module's include lists name is a member of its class, and it and
     every conventions-defined import is in both files; the files contain nothing else. The
     names of the members are derived from the RTTI here, independently of the generator's
     emitters. Members that no include list names are not added yet, which is not a failure.
  3. Every released import whose member is deprecated is marked deprecated in the checked-in
     record, and every entry of DEPRECATED names a member.
  4. Regenerating from the RTTI at D0 changes only imports whose presence a compatibility flag
     gates: no import changes signature, and every changed line of the files belongs to an import
     listed in compat-gates.json (or is the compat date stamp).
"""

import argparse
import contextlib
import difflib
import json
import re
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).absolute().parent
sys.path.insert(0, str(HERE))

import dump_rtti
import generate
import rtti_reader
import typescript

REGENERATE = (
    "bazel build //src/workerd/server:workerd && python3 -I "
    "src/workerd/wasm-abi/generator/generate.py --workerd bazel-bin/src/workerd/server/workerd "
    "--compat-date {date}"
)

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL:", msg)


def checked_in(name):
    s = generate.stem(name)
    return generate.RECORDS / (s + ".json"), generate.SHIMS / (s + ".js")


def shim_names(text):
    return set(re.findall(r"^ {6}n: '([^']+)',$", text, re.MULTILINE))


def member_names(rtti, spec):
    """Import names of every member of a module's classes, by class, straight from the RTTI and
    the naming rules."""
    by_name = rtti_reader.structures_by_name(rtti)
    out = {}
    for fqn, _ in spec.structures:
        s = by_name[fqn]
        names = out[fqn] = set()
        js = "globalThis" if fqn == generate.GLOBAL_SCOPE else s["name"]
        for member in s["members"]:
            ((kind, m),) = member.items()
            if kind == "method":
                if js == "globalThis":
                    names.add("globalThis." + m["name"])
                else:
                    sep = "." if m["static"] else "#"
                    names.add(f"{js}{sep}{m['name']}")
            elif kind == "constructor" and js != "globalThis":
                names.add(js + ".constructor")
            elif kind == "property":
                names.add("{}#{}.get".format(js, m["name"]))
                if not m["readonly"]:
                    names.add("{}#{}.set".format(js, m["name"]))
    return out


def explain_staleness(name, old_text, new_text):
    """Says what changed between the checked-in and the generated record of a module."""
    old = {i["name"]: i for i in json.loads(old_text)["imports"]}
    new = {i["name"]: i for i in json.loads(new_text)["imports"]}
    for n, imp in new.items():
        if n not in old:
            print(f"     new import {n}: its JSG member is new")
        elif imp.get("deprecated") and not old[n].get("deprecated"):
            print(
                f"     {n} is now deprecated ({imp['deprecated']}). It stays, because "
                "released imports are permanent; the regenerated record marks it deprecated"
            )
        elif imp != old[n]:
            fields = sorted(k for k in imp if imp.get(k) != old[n].get(k))
            print(f"     {n} changed in {', '.join(fields)}")


def check_fresh(outputs, date):
    for name, o in outputs.items():
        record, shim = checked_in(name)
        for path, text in ((record, o.record), (shim, o.shim)):
            rel = path.relative_to(generate.HERE.parent)
            if not path.exists() or path.read_text() != text:
                fail(
                    f"{rel} is stale; regenerate the modules and commit them with this "
                    f"change: {REGENERATE.format(date=date)}"
                )
                if path == record and path.exists():
                    explain_staleness(name, path.read_text(), text)
            else:
                print(f"ok   {rel} matches a fresh generation")


def check_coverage(rtti, verbose):
    for spec in generate.MODULES:
        record_path, shim_path = checked_in(spec.name)
        if not record_path.exists() or not shim_path.exists():
            continue  # Reported as stale.
        before = len(failures)
        record = json.loads(record_path.read_text())
        in_json = {i["name"] for i in record["imports"]}
        in_shim = shim_names(shim_path.read_text())
        by_class = member_names(rtti, spec)
        members = set().union(*by_class.values())
        included = [n for _, include in spec.structures for n in include]
        expected = included + [g["name"] for g in spec.defined]
        for n in included:
            if n not in members:
                fail(f"{spec.name}: the include list names {n}, which is not a member")
        for n in expected:
            missing = [
                f
                for f, ok in (("record", n in in_json), ("shim", n in in_shim))
                if not ok
            ]
            if missing:
                fail(f"{spec.name}: {n} missing from the {' and '.join(missing)}")
            elif verbose:
                print(f"     {spec.name} {n}")
        extra = (in_json | in_shim) - set(expected)
        if extra:
            fail(f"{spec.name}: imports that no include list names: {sorted(extra)}")
        excluded = {e["name"] for e in record.get("excluded", [])}
        for n in sorted(excluded - set(generate.DEPRECATED)):
            fail(f"{spec.name}: {n} is excluded, but not listed in DEPRECATED")
        # The global scope is not a class of the module; it only provides its global functions.
        not_added = set().union(
            *(n for fqn, n in by_class.items() if fqn != generate.GLOBAL_SCOPE)
        )
        not_added -= set(included) | excluded
        if verbose:
            for n in sorted(not_added):
                print(f"     {spec.name} {n} (not added yet)")
        if len(failures) == before:
            print(
                f"ok   {spec.name}: {len(in_json)} imports; of the members of its classes, "
                f"{len(not_added)} are not added yet and {len(excluded)} excluded"
            )


def check_deprecation(outputs):
    """Every released import whose member is deprecated is marked deprecated in the checked-in
    record."""
    before = len(failures)
    for name, o in outputs.items():
        record_path, _ = checked_in(name)
        if not record_path.exists():
            continue
        record = {i["name"]: i for i in json.loads(record_path.read_text())["imports"]}
        for imp in o.module.imports:
            reason = imp.get("deprecated")
            if reason and not record.get(imp["name"], {}).get("deprecated"):
                fail(
                    f"{name}: {imp['name']} is deprecated ({reason}). It stays, because "
                    "released imports are permanent, but the checked-in record must mark it "
                    "deprecated: regenerate the modules and commit them with this change"
                )
    for n in generate.unused_exclusions(outputs):
        fail(f"DEPRECATED lists {n}, which matches no member; remove the entry")
    if len(failures) == before:
        print("ok   deprecated members are marked, and DEPRECATED names only members")


def signature_of(imp):
    return json.dumps(
        {k: imp[k] for k in ("wasm", "params", "result", "suspending", "ret")},
        sort_keys=True,
    )


def check_dates(new_outputs, old_outputs, new_date, old_date):
    gates = generate.load_gates() or {"gates": {}}
    for name, new_o in new_outputs.items():
        old_o = old_outputs[name]
        new = {i["name"]: i for i in new_o.module.imports}
        old = {i["name"]: i for i in old_o.module.imports}
        added, removed = sorted(set(new) - set(old)), sorted(set(old) - set(new))
        changed = sorted(
            n
            for n in set(new) & set(old)
            if signature_of(new[n]) != signature_of(old[n])
        )
        module_gates = gates["gates"].get(name, {})
        print(f"     {name} {old_date} -> {new_date}: added {added}, removed {removed}")
        if changed:
            fail(f"{name}: signatures changed between compat dates: {changed}")
        if removed:
            fail(
                f"{name}: imports exist at {old_date} but not at {new_date}: {removed}. A "
                "compatibility flag removed or replaced their members, so they are deprecated "
                "(see conventions.md, Deprecated members)"
            )
        ungated = [n for n in added if n not in module_gates]
        if ungated:
            fail(
                f"{name}: imports added without a recorded gating flag: {ungated}; rerun "
                "generator/compat_gates.py (see conventions.md, Tooling)"
            )
        for n in added:
            if n in module_gates:
                g = module_gates[n]
                print(f"     {n} gated by {g['flag']} (enabled {g['enableDate']})")
        allowed = set(added) | set(removed) | {new_date, old_date}
        for kind, old_text, new_text in (
            ("record", old_o.record, new_o.record),
            ("shim", old_o.shim, new_o.shim),
        ):
            check_diff(f"{name} {kind}", old_text, new_text, allowed)


def check_diff(label, old_text, new_text, allowed):
    diff = list(
        difflib.unified_diff(
            old_text.splitlines(), new_text.splitlines(), "old", "new", n=0, lineterm=""
        )
    )
    body = [
        line
        for line in diff
        if line[:1] in "+-" and not line.startswith(("+++", "---"))
    ]
    stray = [
        line
        for line in body
        if not any(a in line for a in allowed)
        and not _in_gated_block(line, diff, allowed)
    ]
    if stray:
        fail(
            "{}: lines changed outside gated imports:\n  {}".format(
                label, "\n  ".join(stray[:10])
            )
        )
    print(f"---- diff {label} ({len(body)} changed lines)")
    for line in diff:
        print("     " + line)


def _in_gated_block(line, diff, allowed):
    """Whether `line` sits in a diff hunk that also names a gated import (multi-line entries)."""
    idx = diff.index(line)
    start = idx
    while start > 0 and not diff[start].startswith("@@"):
        start -= 1
    end = idx
    while end + 1 < len(diff) and not diff[end + 1].startswith("@@"):
        end += 1
    return any(a in hunk_line for hunk_line in diff[start : end + 1] for a in allowed)


@contextlib.contextmanager
def rtti_dumps(args, tmp):
    """Yields the paths of the RTTI dumps at the new and the old date."""
    if args.workerd is None:
        if args.rtti is None or args.old_rtti is None:
            raise SystemExit("pass --workerd, or both --rtti and --old-rtti")
        yield args.rtti, args.old_rtti
        return
    paths = []
    with dump_rtti.rtti_server(args.workerd) as get:
        for date in (args.compat_date, args.old_date):
            path = tmp / (date + ".capnp.bin")
            path.write_bytes(get(date))
            paths.append(path)
    yield tuple(paths)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workerd", help="workerd binary to dump the RTTI with")
    ap.add_argument("--rtti", help="RTTI dump at --compat-date")
    ap.add_argument("--compat-date", required=True)
    ap.add_argument("--old-rtti", help="RTTI dump at --old-date")
    ap.add_argument("--old-date", required=True)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    ts = typescript.Declarations.load()
    gates = generate.load_gates()
    with (
        tempfile.TemporaryDirectory() as tmp,
        rtti_dumps(args, Path(tmp)) as (new_path, old_path),
    ):
        new_rtti = rtti_reader.read_rtti(new_path)
        old_rtti = rtti_reader.read_rtti(old_path)
    try:
        new = generate.generate(new_rtti, args.compat_date, ts=ts, gates=gates)
        old = generate.generate(
            old_rtti, args.old_date, ts=ts, gates=gates, complete=False
        )
    except generate.GenerationError as e:
        fail(f"the generator refuses to generate the modules:\n{e}")
    else:
        check_fresh(new, args.compat_date)
        check_coverage(new_rtti, args.verbose)
        check_deprecation(new)
        check_dates(new, old, args.compat_date, args.old_date)
    if failures:
        print(f"\n{len(failures)} failure(s)")
        sys.exit(1)
    print("\nself-test passed")


if __name__ == "__main__":
    main()
