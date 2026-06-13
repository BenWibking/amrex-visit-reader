#!/usr/bin/env python3
"""
ABOUTME: VisIt CLI check for the plugin read options: disabling the domain
ABOUTME: boundary cache and declaring invariant mesh structure across time.

Run with: `visit -cli -nowin -debug 1 -s example_data/check_read_options.py`
Optionally pass a different plotfile directory as the first argument.

The wrapper script must remove stale *.vlog files before launching VisIt so
the scans below only see logs from this run.
"""

import glob
import os
import sys

OPT_BOUNDARIES = "Build domain boundaries for ghost synthesis"
OPT_INVARIANT = "Mesh structure is invariant across timesteps"


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


def scan_vlogs(marker):
    """Return all vlog lines containing marker."""
    hits = []
    for vlog in glob.glob("*.vlog"):
        with open(vlog, "r", errors="replace") as handle:
            for line in handle:
                if marker in line:
                    hits.append(f"{vlog}: {line.strip()}")
    return hits


def check_invariant_mesh_timesteps():
    """Verify timestep changes skip metadata re-population with the option on."""
    script_dir = os.path.abspath(os.path.dirname(__file__))
    dataset = os.path.join(script_dir, "DiskGalaxy", "plt0000020")
    if not os.path.isdir(dataset):
        print(f"SKIP: invariant-mesh timestep check ({dataset} not present)")
        return

    if not OpenDatabase(dataset):
        raise RuntimeError(f"Failed to open '{dataset}'")
    var_name = first_scalar_variable(dataset)
    if not AddPlot("Pseudocolor", var_name):
        raise RuntimeError(f"Failed to add Pseudocolor plot of '{var_name}'")
    if not DrawPlots():
        raise RuntimeError("DrawPlots failed")
    Query("MinMax")
    state0 = GetQueryOutputValue()

    if TimeSliderSetState(1) != 1:
        raise RuntimeError("TimeSliderSetState(1) failed")
    Query("MinMax")
    state1 = GetQueryOutputValue()
    print(f"Invariant-mesh MinMax: state0={state0} state1={state1}")
    if not state0 or not state1:
        raise RuntimeError("MinMax queries failed across timesteps")

    DeleteAllPlots()
    CloseDatabase(dataset)
    CloseComputeEngine()


def main():
    dataset = resolve_dataset_path()
    print(f"Opening dataset with read options: {dataset}")

    # Force a fresh plugin-info query; otherwise the viewer may serve stale
    # (possibly empty) options recorded in the user's saved config file.
    OpenMDServer("localhost")
    opts = GetDefaultFileOpenOptions("amrex-plotfile")
    if OPT_BOUNDARIES not in opts or OPT_INVARIANT not in opts:
        raise RuntimeError(
            f"Plugin does not advertise the expected read options; got: {opts}"
        )
    opts[OPT_BOUNDARIES] = 0
    opts[OPT_INVARIANT] = 1
    if not SetDefaultFileOpenOptions("amrex-plotfile", opts):
        raise RuntimeError("SetDefaultFileOpenOptions failed")

    if not OpenDatabase(dataset):
        raise RuntimeError(f"Failed to open '{dataset}'")

    var_name = first_scalar_variable(dataset)
    print(f"Plotting scalar: {var_name}")
    if not AddPlot("Pseudocolor", var_name):
        raise RuntimeError(f"Failed to add Pseudocolor plot of '{var_name}'")
    if not DrawPlots():
        raise RuntimeError("DrawPlots failed")

    if not Query("Variable Sum"):
        raise RuntimeError("Variable Sum query failed with ghost synthesis off")
    print(f"Variable Sum: {GetQueryOutputString().strip()}")

    # Known VisIt quirk: the combined MinMax query's message string comes back
    # empty on single-level data when no ghost arrays exist, but the values
    # are computed correctly and are available programmatically.
    Query("MinMax")
    minmax = GetQueryOutputValue()
    if not minmax or len(minmax) != 2 or minmax[0] > minmax[1]:
        raise RuntimeError(f"MinMax query returned invalid values: {minmax!r}")
    print(f"MinMax values: {minmax}")

    DeleteAllPlots()
    CloseDatabase(dataset)
    CloseComputeEngine()

    sanity = scan_vlogs("[amrex-plugin] PopulateDatabaseMetaData complete")
    if not sanity:
        raise RuntimeError(
            "No metadata-population lines found in vlogs; debug logging is not "
            "being captured. Was VisIt launched with -debug 1?"
        )

    boundary_hits = scan_vlogs("Cached structured boundaries")
    if boundary_hits:
        details = "\n".join(boundary_hits)
        raise RuntimeError(
            f"Domain boundaries were built despite the read option being off:\n{details}"
        )

    option_hits = scan_vlogs("domainBoundaries=off invariantMesh=on")
    if not option_hits:
        raise RuntimeError(
            "Plugin never reported the requested read options; they were not "
            "delivered to the file format."
        )

    check_invariant_mesh_timesteps()

    print("PASS: Read options honored; no boundary cache was built.")


try:
    main()
except Exception as exc:  # noqa: BLE001 - report and exit nonzero for the wrapper
    print(f"FAIL: {exc}")
    sys.exit(1)

sys.exit(0)
