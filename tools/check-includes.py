#!/usr/bin/env python3
"""usage: tools/check-includes.py [src-dir]

Checks that the compositor's subsystems do not depend on each other in a
circle. src-dir defaults to the checkout's src/. Exits 1 on a finding.

  1. No header includes itself through others: the include graph of the
     headers under src/ has no cycle. Each cycle is printed as its chain.
  2. A subsystem that others call and that must call none of them back
     (ONE_WAY below) neither includes their headers nor names their `Server`
     members. A `Server` member is reached through server.h, which includes
     everything, so the include graph alone cannot see that call.
"""

import re
import sys
from pathlib import Path

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)

# One-way subsystems: its sources (paths below src/, a directory with a
# trailing slash), the sources of that directory it does not cover, the
# `Server` members it must not name and the headers it must not include.
# Input is the seat and the dispatch: the session lock and the control
# socket call it, and what it must tell them goes through InputGate or a
# callback its caller hands it.
ONE_WAY = {
    "Input": {
        "sources": ["input/"],
        "apart": ["input/bindings.", "input/gesture.", "input/key_syms.h"],
        "members": ["lock", "ipc", "ipc_events"],
        "headers": [r"platform/lock\.h", r"platform/ipc_[a-z_]*\.h"],
    },
}


def sources(src):
    return sorted(p for p in src.rglob("*") if p.suffix in (".h", ".cpp"))


def includes(src, path):
    """The headers under src/ that `path` includes, as src-relative names."""
    found = []
    for name in INCLUDE.findall(path.read_text()):
        for base in (src, path.parent):
            if (base / name).is_file():
                found.append((base / name).relative_to(src).as_posix())
                break
    return found


def cycles(graph):
    """One chain for each strongly connected group of more than one header, or
    for a header that includes itself."""
    index, low, stack, on, groups = {}, {}, [], set(), []

    def visit(node):
        index[node] = low[node] = len(index)
        stack.append(node)
        on.add(node)
        for next_ in graph[node]:
            if next_ not in index:
                visit(next_)
                low[node] = min(low[node], low[next_])
            elif next_ in on:
                low[node] = min(low[node], index[next_])
        if low[node] == index[node]:
            group = []
            while True:
                member = stack.pop()
                on.discard(member)
                group.append(member)
                if member == node:
                    break
            if len(group) > 1 or node in graph[node]:
                groups.append(group)

    for node in sorted(graph):
        if node not in index:
            visit(node)
    return [chain(graph, sorted(group)) for group in groups]


def chain(graph, group):
    """The shortest path through the group that leaves its first header and
    returns to it."""
    start = group[0]
    came = {start: None}
    queue = [start]
    while queue:
        here = queue.pop(0)
        for next_ in sorted(graph[here]):
            if next_ == start:
                path = [here]
                while came[path[-1]] is not None:
                    path.append(came[path[-1]])
                return path[::-1] + [start]
            if next_ in group and next_ not in came:
                came[next_] = here
                queue.append(next_)
    return group


def one_way(src, files):
    problems = []
    for subsystem, rule in ONE_WAY.items():
        member = re.compile(r"\bserver_?\s*(?:\.|->)\s*(%s)\b" % "|".join(rule["members"]))
        header = re.compile(r'^\s*#\s*include\s+"(?:%s)"' % "|".join(rule["headers"]), re.M)
        for path in files:
            name = path.relative_to(src).as_posix()
            if not any(name.startswith(s) for s in rule["sources"]):
                continue
            if any(name.startswith(a) for a in rule["apart"]):
                continue
            for number, line in enumerate(path.read_text().splitlines(), 1):
                found = member.search(line) or header.search(line)
                if found:
                    problems.append(f"{name}:{number}: {subsystem} reaches {found.group(0).strip()}")
    return problems


def main():
    if sys.argv[1:2] in (["-h"], ["--help"]):
        print(__doc__.strip())
        return 0
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "src"
    if not src.is_dir():
        print(f"check-includes: no such directory: {src}", file=sys.stderr)
        return 2
    files = sources(src)
    headers = [p for p in files if p.suffix == ".h"]
    graph = {p.relative_to(src).as_posix(): set() for p in headers}
    for path in headers:
        name = path.relative_to(src).as_posix()
        graph[name].update(includes(src, path))
    problems = ["include cycle: " + " -> ".join(c) for c in cycles(graph)]
    problems += one_way(src, files)
    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        return 1
    print(f"check-includes: {len(headers)} headers, no cycle; one-way subsystems hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
