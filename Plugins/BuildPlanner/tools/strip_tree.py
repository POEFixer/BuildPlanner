#!/usr/bin/env python3
"""BuildPlanner. Makes data/tree.json from Path of Building (PoE2)'s passive tree.

Usage:
    python -I tools/strip_tree.py <PoB src/TreeData/<v>/tree.json> <out tree.json> --version 0_5 --commit <sha>

Path of Building Community (PoE2), github.com/PathOfBuildingCommunity/PathOfBuilding-PoE2, ships one
tree.json per tree version (src/TreeData/0_1 .. 0_5). The plugin's loader (src/core/TreeData.cpp) reads
that format directly, so a tree.json the user downloads later ("Update tree data") parses with the same
code. This tool only drops what the plugin never reads (sprite and art references, oils, flavour text,
the stats of plain small nodes, class backgrounds, the bounding box) and adds a "bp_meta" object naming
the source, the tree version and the Path of Building commit.

Standard library only, no network. Deterministic: sorted keys, no timestamps, the same input gives the
same bytes.
"""
import argparse
import json
import re
import sys

# Node keys the loader reads. Everything else (icon, recipe, activeEffectImage, nodeOverlay,
# connectionArt, flavourText, applyToArmour, aliasPassiveSocket, noRadius, sinister) is art or text
# the plugin never shows.
KEEP_NODE = (
    "skill", "stringId", "name", "stats", "group", "orbit", "orbitIndex", "connections",
    "isNotable", "isKeystone", "isJewelSocket", "isOnlyImage", "isAttribute", "options",
    "isAscendancyStart", "ascendancyName", "classesStart", "isMultipleChoice",
    "isMultipleChoiceOption", "isFreeAllocate", "isSwitchable", "unlockConstraint",
    "containJewelSocket",
)
KEEP_CONSTANTS = ("orbitRadii", "skillsPerOrbit", "orbitAnglesByOrbit")
VERSION_RE = re.compile(r"^\d+_\d+$")


def important(node):
    """Nodes whose stats the plugin shows (tooltips, the Passives tab). The game shows the stats of
    plain small nodes in its own tooltip."""
    return bool(node.get("isNotable") or node.get("isKeystone") or node.get("isJewelSocket")
                or node.get("ascendancyName"))


def strip_options(node, keep_stats):
    opts = node.get("options")
    if node.get("isAttribute") and isinstance(opts, list):
        # "+5 to any Attribute": the option ids (what the game stores as the choice) and names.
        return [{k: o[k] for k in ("id", "name") if k in o} for o in opts if isinstance(o, dict)]
    if node.get("isSwitchable") and isinstance(opts, dict):
        out = {}
        for key, o in opts.items():
            if not isinstance(o, dict):
                continue
            keep = ("id", "name", "ascendancyName") + (("stats",) if keep_stats else ())
            out[key] = {k: o[k] for k in keep if k in o}
        return out
    return None


def strip_node(node):
    out = {k: node[k] for k in KEEP_NODE if k in node and k != "options"}
    keep_stats = important(node) and not node.get("isOnlyImage")
    if not keep_stats:
        out.pop("stats", None)
    if "connections" in out:
        out["connections"] = [{"id": c["id"], "orbit": c.get("orbit", 0)}
                              for c in out["connections"] if isinstance(c, dict) and "id" in c]
    opts = strip_options(node, keep_stats)
    if opts is not None:
        out["options"] = opts
    return out


def strip_class(c):
    out = {"name": c.get("name", "")}
    if "integerId" in c:
        out["integerId"] = c["integerId"]
    out["ascendancies"] = [{k: a[k] for k in ("id", "name", "internalId") if k in a}
                           for a in c.get("ascendancies", []) if isinstance(a, dict)]
    return out


def strip_group(g):
    if not isinstance(g, dict):
        return None   # PoB's group list has holes: the index is the node's group - 1
    return {k: g[k] for k in ("x", "y") if k in g}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--version", required=True, help='the tree version folder, e.g. "0_5"')
    ap.add_argument("--commit", default="", help="the Path of Building (PoE2) commit the file came from")
    a = ap.parse_args()
    if not VERSION_RE.match(a.version):
        sys.exit("--version must look like 0_5")

    with open(a.src, encoding="utf-8") as f:
        d = json.load(f)
    if not isinstance(d.get("nodes"), dict) or "groups" not in d or not isinstance(d.get("constants"), dict):
        sys.exit("not a Path of Building tree.json (nodes / groups / constants missing)")

    groups = d["groups"]
    out = {
        "bp_meta": {
            "source": "Path of Building Community (PoE2), src/TreeData/%s/tree.json "
                      "(github.com/PathOfBuildingCommunity/PathOfBuilding-PoE2); "
                      "passive tree data (c) Grinding Gear Games" % a.version,
            "treeVersion": a.version,
            "commit": a.commit,
        },
        "classes": [strip_class(c) for c in d.get("classes", []) if isinstance(c, dict)],
        "constants": {k: v for k, v in d["constants"].items() if k in KEEP_CONSTANTS},
        "groups": ([strip_group(g) for g in groups] if isinstance(groups, list)
                   else {k: strip_group(g) for k, g in groups.items()}),
        "jewelSlots": d.get("jewelSlots", []),
        "nodes": {k: strip_node(v) for k, v in d["nodes"].items() if isinstance(v, dict)},
    }
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        f.write("\n")
    print("%s: %d nodes, %d groups, %d classes, tree %s" %
          (a.out, len(out["nodes"]), len(out["groups"]), len(out["classes"]), a.version))


if __name__ == "__main__":
    main()
