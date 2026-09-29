#!/usr/bin/env python3
"""Report verified decomp metrics and rank the remaining source-link work.

Example (after a full GMSE01 build in the decomp checkout):
  python3 tools/decomp_status.py --decomp ../sms \
      --markdown docs/DECOMP_STATUS.md --queue build/decomp-remaining.tsv

This reads objdiff's report and the source-link manifest; it never changes
matching status or treats the original-object fallback as decompiled code.
"""

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess


def commit(path):
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()


def load_status(root, version):
    report_path = root / "build" / version / "report.json"
    report = json.loads(report_path.read_text())
    linked = {str(Path(p).with_suffix("")) for p in json.loads(
        (root / "config" / version / "objects.json").read_text())}
    target = (root / "config" / version / "build.sha1").read_text().split()[0]
    dol_path = root / "build" / version / "mario.dol"
    dol_hash = hashlib.sha1(dol_path.read_bytes()).hexdigest()
    units = report["units"]
    pending = []
    for unit in units:
        name = unit["name"].removeprefix("mario/")
        if name in linked:
            continue
        measures = unit["measures"]
        nonexact = [f for f in unit.get("functions", [])
                    if float(f.get("fuzzy_match_percent", 0)) < 100]
        pending.append({
            "unit": name,
            "nonexact_functions": len(nonexact),
            "unmatched_code_bytes": int(measures.get("total_code", 0))
                                    - int(measures.get("matched_code", 0)),
            "data_match_percent": float(measures.get("matched_data_percent", 0)),
            "functions": nonexact,
        })
    pending.sort(key=lambda u: (u["nonexact_functions"],
                               u["unmatched_code_bytes"], u["unit"]))
    complete = (dol_hash == target and not pending
                and int(report["measures"]["matched_code"])
                == int(report["measures"]["total_code"])
                and int(report["measures"]["matched_data"])
                == int(report["measures"]["total_data"]))
    return report, linked, pending, dol_hash, target, complete, report_path


def render(root, version, report, linked, pending, dol_hash, target,
           complete, report_path, port_pin):
    measures = report["measures"]
    rows = []
    for category in report["categories"]:
        cid, m = category["id"], category["measures"]
        units = [u for u in report["units"] if cid in
                 u.get("metadata", {}).get("progress_categories", [])]
        count = sum(u["name"].removeprefix("mario/") in linked for u in units)
        rows.append((category["name"], m, count, len(units)))
    rows.append(("All", measures, len(linked), len(report["units"])))
    source_commit = commit(root)
    stamp = datetime.fromtimestamp(report_path.stat().st_mtime,
                                   timezone.utc).isoformat(timespec="seconds")
    lines = ["# Decompilation status", "",
             f"Target: `{version}`. Decomp checkout: `{source_commit}`.",
             f"Build report generated: {stamp}.",
             f"Port decomp pin: `{port_pin}`.",
             f"Completion: **{'all reported units source-linked and matching' if complete else 'incomplete'}**.", "",
             "| Category | Exact code | Source-linked code | Source-linked units |",
             "| --- | ---: | ---: | ---: |"]
    for name, m, count, total in rows:
        lines.append(f"| {name} | {m['matched_code_percent']:.5f}% | "
                     f"{m['complete_code_percent']:.5f}% | {count} / {total} |")
    remaining = int(measures["total_code"]) - int(measures["matched_code"])
    lines += ["", f"Exact functions: {int(measures['matched_functions']):,} / "
              f"{int(measures['total_functions']):,}.",
              f"Exact data: {measures['matched_data_percent']:.5f}%.",
              f"Code outside exact functions: {remaining:,} bytes.",
              f"Units awaiting source linking: {len(pending)}.", "",
              f"Rebuilt DOL SHA-1: `{dol_hash}`.",
              f"Original DOL hash verification: **{'PASS' if dol_hash == target else 'FAIL'}**.", "",
              "A matching DOL can still contain original binary objects.",
              "Completion requires closing and source-linking every remaining unit; the DOL hash alone is insufficient.", "",
              "## Closest units to source linking", "",
              "Use this fresh ranking to choose experiments and compare source directly with the original binary.",
              "Historical notes can supply hypotheses; new compiler and binary evidence determines the result.", "",
              "| Unit | Non-exact functions | Code outside exact functions | Exact data |",
              "| --- | ---: | ---: | ---: |"]
    for u in pending[:30]:
        lines.append(f"| `{u['unit']}` | {u['nonexact_functions']} | "
                     f"{u['unmatched_code_bytes']:,} B | {u['data_match_percent']:.5f}% |")
    lines += ["", "Regenerate after a successful build:", "", "```sh",
              "python3 tools/decomp_status.py --decomp ../sms --markdown docs/DECOMP_STATUS.md --queue build/decomp-remaining.tsv",
              "```", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--decomp", type=Path, required=True)
    parser.add_argument("--version", default="GMSE01")
    parser.add_argument("--markdown", type=Path)
    parser.add_argument("--queue", type=Path)
    args = parser.parse_args()
    root = args.decomp.resolve()
    state = load_status(root, args.version)
    port_root = Path(__file__).resolve().parents[1]
    text = render(root, args.version, *state, commit(port_root / "decomp"))
    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(text)
    else:
        print(text)
    if args.queue:
        args.queue.parent.mkdir(parents=True, exist_ok=True)
        with args.queue.open("w", newline="") as output:
            writer = csv.writer(output, delimiter="\t")
            writer.writerow(["unit", "nonexact_functions", "unmatched_code_bytes",
                             "data_match_percent", "symbol", "function_bytes",
                             "fuzzy_match_percent"])
            for u in state[2]:
                functions = sorted(u["functions"],
                                   key=lambda f: (-int(f.get("size", 0)), f["name"]))
                for f in functions or [{}]:
                    writer.writerow([u["unit"], u["nonexact_functions"],
                                     u["unmatched_code_bytes"], u["data_match_percent"],
                                     f.get("name", ""), f.get("size", ""),
                                     f.get("fuzzy_match_percent", "")])
    if state[3] != state[4]:
        parser.exit(1, "Rebuilt DOL differs from the original.\n")


if __name__ == "__main__":
    main()
