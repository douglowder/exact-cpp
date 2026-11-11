#!/usr/bin/env python3
"""The C++ route port (rt/src/rt_route.cpp) against Exact's route corpus.

Usage:
  route_corpus.py ROUTE_CHECK CORPUS.json

ROUTE_CHECK is rt-test/build/route_check (or its -san build); CORPUS is
Exact's route/tests/corpus.json, whose expected values come from Chrome and
the Rust crate. Every record is run except `checks` (the compiler's static
table rejects, not ported) and `laws` with their `law_locations` (properties
of the same verbs that the sequences already pin); `oracle` and `decisions`
describe the corpus and hold no cases. Exit 0 when every answer matches.
"""

from __future__ import annotations

import json
import subprocess
import sys


def hx(s: str) -> str:
    return s.encode().hex() or "-"


class Check:
    """One route_check process."""

    def __init__(self, binary: str) -> None:
        self.p = subprocess.Popen(
            [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True
        )

    def ask(self, *lines: str) -> object:
        assert self.p.stdin and self.p.stdout
        for line in lines:
            self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def close(self) -> int:
        assert self.p.stdin
        self.p.stdin.close()
        return self.p.wait(timeout=10)


def load_tables(c: Check, tables: dict) -> None:
    for name, table in tables.items():
        rows = [
            f"R {hx(r['name'])} {hx(r['pattern'])} "
            f"{-1 if r['parent'] is None else r['parent']} {int(r['tab'])} {int(r['notfound'])}"
            for r in table["routes"]
        ]
        c.ask(f"T {hx(name)} {len(rows)}", *rows)


def run(c: Check, corpus: dict) -> list[str]:
    fails: list[str] = []
    count = 0

    def expect(label: str, actual: object, expected: object) -> None:
        nonlocal count
        count += 1
        if actual != expected:
            fails.append(f"{label}: {actual!r} != {expected!r}")

    load_tables(c, corpus["tables"])
    for name, names in corpus["param_names"].items():
        expect(f"param_names {name}", c.ask(f"names {hx(name)}"), names)
    for op in ("canonical", "location_of"):
        for case in corpus[op]:
            expect(f"{op} {case['input']!r}", c.ask(f"{op} {hx(case['input'])}"), case["expected"])
    for case in corpus["fragments"]:
        expect(f"fragment_of {case['input']!r}", c.ask(f"fragment_of {hx(case['input'])}"), case["expected"])
    for case in corpus["components"]:
        expect(f"component {case['input']!r}", c.ask(f"component {hx(case['input'])}"), case["expected"])
    for case in corpus["search"]:
        got = c.ask(f"search {hx(case['url'])} {hx(case['name'])}")
        expect(f"search {case['url']!r} {case['name']!r}", got, case["expected"])
    for case in corpus["matches"]:
        t, loc = hx(case["table"]), hx(case["location"])
        expect(f"match {case['table']} {case['location']!r}", c.ask(f"match {t} {loc}"), case["expected"])
        routes = corpus["tables"][case["table"]]["routes"]
        want = case["expected"]
        if want is not None and not any(r["name"] == want["name"] and not r["notfound"] for r in routes):
            want = None
        expect(f"pattern {case['table']} {case['location']!r}", c.ask(f"pattern {t} {loc}"), want)
    for case in corpus["chains"]:
        got = c.ask(f"chain {hx(case['table'])} {hx(case['location'])}")
        expect(f"chain {case['table']} {case['location']!r}", got, case["expected"])
    for case in corpus["paths"]:
        args = " ".join(hx(p) for p in case["params"])
        got = c.ask(f"path {hx(case['table'])} {hx(case['name'])} {args}".rstrip())
        expect(
            f"path {case['table']} {case['name']} {case['params']!r}",
            got,
            {"value": case["expected"], "error": case["error"]},
        )
    reads = {}
    for case in corpus["reads"]:
        reads.setdefault((case["sequence"], case["step"]), []).append(case)
    for si, seq in enumerate(corpus["sequences"]):
        c.ask(f"seq {hx(seq['table'])}")
        for i, step in enumerate(seq["steps"]):
            arg = "" if step.get("arg") is None else " " + hx(step["arg"])
            got = c.ask(f"step {step['verb']}{arg}")
            label = f"{seq['name']} step {i} {step['verb']} {step.get('arg')!r}"
            expect(label, got, {"router": step["expected"], "refusal": step.get("refusal")})
            r = step["expected"]
            selected = next(t["stack"] for t in r["tabs"] if t["name"] == r["tab"])
            want = {"stack": selected, "top": selected[-1] if selected else None, "depth": len(selected)}
            expect(f"{label} reads", c.ask("reads"), want)
            for case in reads.get((si, i), []):
                expect(f"{label} params {case['name']}", c.ask(f"params {hx(case['name'])}"), case["expected"])
    print(f"route_corpus: {count} answers checked")
    return fails


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    with open(sys.argv[2], encoding="utf-8") as f:
        corpus = json.load(f)
    c = Check(sys.argv[1])
    fails = run(c, corpus)
    code = c.close()
    for fail in fails:
        print(f"FAIL {fail}")
    if code != 0:
        fails.append(f"route_check exited {code}")
    print("route_corpus: " + ("all match" if not fails else f"{len(fails)} failed"))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
