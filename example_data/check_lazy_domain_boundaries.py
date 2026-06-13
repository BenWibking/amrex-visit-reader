#!/usr/bin/env python3
"""
ABOUTME: VisIt CLI check that domain boundary info is never built in the
ABOUTME: mdserver and is built in the engine by default; scans vlogs to verify.

Run with: `visit -cli -nowin -debug 1 -s example_data/check_lazy_domain_boundaries.py`
Optionally pass a different plotfile directory as the first argument.

The wrapper script must remove stale *.vlog files before launching VisIt so
the scan below only sees logs from this run.
"""

import glob
import os
import sys


def resolve_dataset_path():
    """Return the dataset path from argv or fall back to the default 3D sample."""
    script_dir = os.path.abspath(os.path.dirname(__file__))
    default_dataset = os.path.join(script_dir, "Nyx_LyA", "plt00000")
    dataset = default_dataset
    if len(sys.argv) > 1:
        dataset = sys.argv[1]
    dataset = os.path.abspath(dataset)
    if not os.path.isdir(dataset):
        raise RuntimeError(f"Dataset '{dataset}' does not exist or is not a directory")
    return dataset


def first_scalar_variable(dataset):
    """Return the first scalar variable advertised in the metadata."""
    metadata = GetMetaData(dataset)
    if metadata.GetNumScalars() < 1:
        raise RuntimeError("Dataset advertises no scalar variables")
    return metadata.GetScalars(0).name


def scan_vlogs():
    """Scan vlogs for boundary builds; return (mdserver_hits, engine_hits, sanity_hits)."""
    boundary_marker = "Cached structured boundaries"
    sanity_marker = "[amrex-plugin] PopulateDatabaseMetaData complete"
    mdserver_hits = []
    engine_hits = []
    sanity_hits = []
    for vlog in glob.glob("*.vlog"):
        is_mdserver = "mdserver" in vlog
        is_engine = "engine" in vlog
        with open(vlog, "r", errors="replace") as handle:
            for line in handle:
                if boundary_marker in line:
                    if is_mdserver:
                        mdserver_hits.append(f"{vlog}: {line.strip()}")
                    elif is_engine:
                        engine_hits.append(f"{vlog}: {line.strip()}")
                if sanity_marker in line:
                    sanity_hits.append(vlog)
    return mdserver_hits, engine_hits, sanity_hits


def main():
    dataset = resolve_dataset_path()
    print(f"Opening dataset: {dataset}")
    if not OpenDatabase(dataset):
        raise RuntimeError(f"Failed to open '{dataset}'")

    var_name = first_scalar_variable(dataset)
    print(f"Plotting scalar: {var_name}")
    if not AddPlot("Pseudocolor", var_name):
        raise RuntimeError(f"Failed to add Pseudocolor plot of '{var_name}'")
    if not DrawPlots():
        raise RuntimeError("DrawPlots failed")

    Query("MinMax")
    print(f"MinMax result: {GetQueryOutputString().strip()}")

    DeleteAllPlots()
    CloseDatabase(dataset)
    CloseComputeEngine()

    mdserver_hits, engine_hits, sanity_hits = scan_vlogs()
    if not sanity_hits:
        raise RuntimeError(
            "No '[amrex-plugin] PopulateDatabaseMetaData complete' lines found in "
            "vlogs; debug logging is not being captured, so the boundary check "
            "cannot be trusted. Was VisIt launched with -debug 1?"
        )
    if mdserver_hits:
        details = "\n".join(mdserver_hits)
        raise RuntimeError(
            f"Domain boundaries were built in the mdserver:\n{details}"
        )
    if not engine_hits:
        raise RuntimeError(
            "Domain boundaries were not built in the engine even though the "
            "read option defaults to on."
        )

    print("PASS: Boundaries built in engine only, never in the mdserver.")


try:
    main()
except Exception as exc:  # noqa: BLE001 - report and exit nonzero for the wrapper
    print(f"FAIL: {exc}")
    sys.exit(1)

sys.exit(0)
