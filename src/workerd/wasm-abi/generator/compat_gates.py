# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Finds the compatibility flag that gates each import whose presence differs between two
compatibility dates, and writes the result to `compat-gates.json`, which `generate.py` folds
into the module records.

JSG RTTI is exported for one compatibility date and flag set and does not record which flag
gates a member, so this script asks workerd: for every flag enabled between the two dates, it
dumps RTTI at the older date plus that flag and records which imports appear.

Usage:
  python3 -I compat_gates.py --workerd PATH --old-date D0 --new-date D1
"""

import argparse
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).absolute().parent
sys.path.insert(0, str(HERE))

import dump_rtti
import generate
import jsfmt
import rtti_reader


def names(data, date):
    """The (module, import) pairs generated from an RTTI dump."""
    with tempfile.NamedTemporaryFile(suffix=".capnp.bin") as f:
        f.write(data)
        f.flush()
        outputs = generate.generate(rtti_reader.read_rtti(f.name), date, complete=False)
    return {(m, i["name"]) for m, o in outputs.items() for i in o.module.imports}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workerd", required=True)
    ap.add_argument("--old-date", required=True)
    ap.add_argument("--new-date", required=True)
    ap.add_argument("--out", default=generate.GATES)
    args = ap.parse_args()

    flags = [
        f
        for f in generate.parse_compat_flags()
        if f["flag"]
        and f["enableDate"]
        and args.old_date < f["enableDate"] <= args.new_date
    ]
    found = {}
    with dump_rtti.rtti_server(args.workerd) as get:
        old = names(get(args.old_date), args.old_date)
        new = names(get(args.new_date), args.new_date)
        gated = new - old
        if old - new:
            raise SystemExit(f"imports removed between dates: {sorted(old - new)}")
        for f in flags:
            if not gated - set(found):
                break
            spec = "{}+{}".format(args.old_date, f["flag"])
            for key in (names(get(spec), args.old_date) - old) & gated:
                found.setdefault(
                    key,
                    {
                        "flag": f["flag"],
                        "enableDate": f["enableDate"],
                        "field": f["field"],
                    },
                )
    gates = {}
    for (module, name), g in found.items():
        gates.setdefault(module, {})[name] = g
    missing = sorted(f"{m} {n}" for m, n in gated - set(found))
    out = {
        "oldDate": args.old_date,
        "newDate": args.new_date,
        "gates": gates,
        "unattributed": missing,
    }
    Path(args.out).write_text(jsfmt.json_text(out))
    for (module, name), g in sorted(found.items()):
        print(f"{module} {name:<32} {g['flag']} ({g['enableDate']})")
    for name in missing:
        print(f"{name:<40} no single flag found")


if __name__ == "__main__":
    main()
