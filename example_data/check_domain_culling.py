#!/usr/bin/env python3
"""
ABOUTME: VisIt CLI check that spatial/data extents metadata lets operators
ABOUTME: cull domains before I/O; counts GetMesh calls in the engine vlog.

Run with:
  visit -cli -nowin -debug 1 -s example_data/check_domain_culling.py box
  visit -cli -nowin -debug 1 -s example_data/check_domain_culling.py contour

The default dataset (Nyx_LyA/plt00000) has four level-0 patches splitting the
8x8x8 domain along y and z. A box confined to y<3, z<3 intersects only one
patch; a contour at a value just below the global maximum lies in a strict
subset of the patches. Either way, fewer than four domains must be read.

The wrapper script must remove stale *.vlog files before launching VisIt so
the scan below only sees logs from this run.
"""

import glob
import os
import re
import sys

TOTAL_DOMAINS = 4


def resolve_mode_and_dataset():
    mode = "box"
    args = [a for a in sys.argv[1:] if a]
    if args:
        mode = args[0]
    if mode not in ("box", "contour", "contour_geometry"):
        raise RuntimeError(
            f"Unknown mode '{mode}'; expected 'box', 'contour', or "
            "'contour_geometry'"
        )
    script_dir = os.path.abspath(os.path.dirname(__file__))
    dataset = os.path.join(script_dir, "Nyx_LyA", "plt00000")
    if len(args) > 1:
        dataset = os.path.abspath(args[1])
    if not os.path.isdir(dataset):
        raise RuntimeError(f"Dataset '{dataset}' does not exist or is not a directory")
    return mode, dataset


def domains_read_from_vlogs():
    """Return the set of domain ids the plugin's GetMesh served."""
    pattern = re.compile(r"\[amrex-plugin\] GetMesh timeState=\d+ domain=(\d+) mesh=")
    domains = set()
    for vlog in glob.glob("*.vlog"):
        if "engine" not in vlog:
            continue
        with open(vlog, "r", errors="replace") as handle:
            for line in handle:
                match = pattern.search(line)
                if match:
                    domains.add(int(match.group(1)))
    return domains


def main():
    mode, dataset = resolve_mode_and_dataset()
    print(f"Opening dataset: {dataset} (mode={mode})")
    if not OpenDatabase(dataset):
        raise RuntimeError(f"Failed to open '{dataset}'")

    if mode == "box":
        if not AddPlot("Pseudocolor", "density"):
            raise RuntimeError("Failed to add Pseudocolor plot")
        if not AddOperator("Box"):
            raise RuntimeError("Failed to add Box operator")
        box = BoxAttributes()
        box.amount = box.Some
        box.minx, box.maxx = 0.0, 8.0
        box.miny, box.maxy = 0.0, 3.0
        box.minz, box.maxz = 0.0, 3.0
        SetOperatorOptions(box)
    else:
        if not AddPlot("Contour", "density"):
            raise RuntimeError("Failed to add Contour plot")
        contour = ContourAttributes()
        contour.contourMethod = contour.Value
        if mode == "contour":
            # Per the per-FAB ranges in Level_0/Cell_H, patch 2's maximum is
            # 7.4855e9, so this value excludes patch 2 but lies inside the
            # other three patches' ranges.
            contour.contourValue = (7.5e9,)
        else:
            # Mid-range value inside every patch's range; all domains must be
            # read and the contour must produce geometry.
            contour.contourValue = (6.0e9,)
        SetPlotOptions(contour)

    if not DrawPlots():
        raise RuntimeError("DrawPlots failed")

    Query("NumZones")
    print(f"NumZones: {GetQueryOutputString().strip()}")
    num_zones = GetQueryOutputValue()
    if mode in ("box", "contour_geometry") and (not num_zones or num_zones <= 0):
        raise RuntimeError(
            f"{mode} produced no geometry (NumZones={num_zones!r}); culling "
            "may have removed domains that contain the selection."
        )

    DeleteAllPlots()
    CloseDatabase(dataset)
    CloseComputeEngine()

    domains = domains_read_from_vlogs()
    print(f"Domains read by engine: {sorted(domains)}")
    if not domains:
        raise RuntimeError(
            "No GetMesh calls found in engine vlogs; debug logging is not "
            "being captured. Was VisIt launched with -debug 1?"
        )
    if mode == "contour_geometry":
        if len(domains) != TOTAL_DOMAINS:
            raise RuntimeError(
                f"Expected all {TOTAL_DOMAINS} domains to be read for a "
                f"mid-range contour, got {sorted(domains)}."
            )
        print(f"PASS: mid-range contour read all {TOTAL_DOMAINS} domains "
              "and produced geometry.")
        return

    if len(domains) >= TOTAL_DOMAINS:
        raise RuntimeError(
            f"All {TOTAL_DOMAINS} domains were read; {mode} culling via "
            "extents metadata is not working."
        )

    print(f"PASS: {mode} restricted I/O to {len(domains)} of "
          f"{TOTAL_DOMAINS} domains.")


try:
    main()
except Exception as exc:  # noqa: BLE001 - report and exit nonzero for the wrapper
    print(f"FAIL: {exc}")
    sys.exit(1)

sys.exit(0)
