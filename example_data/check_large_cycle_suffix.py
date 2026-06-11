#!/usr/bin/env python3
# ABOUTME: VisIt CLI regression test for plotfile timestep suffixes larger than LLONG_MAX.
# ABOUTME: Checks the directory is not dropped from the series and its cycle clamps to INT_MAX.
"""
VisIt CLI smoke test for very large plotfile timestep suffixes.

Symlinks a sibling plotfile directory whose numeric suffix is ULLONG_MAX
(20 digits, larger than LLONG_MAX) and verifies that:
  1. the reader includes it in the time series (signed parsing dropped it), and
  2. its cycle value is clamped to INT_MAX instead of wrapping negative.
The symlink is removed on exit.

Run with: `visit -cli -nowin -s example_data/check_large_cycle_suffix.py`
Optionally pass a different plotfile directory as the first argument.
"""

import os
import sys

HUGE_SUFFIX = "18446744073709551615"  # ULLONG_MAX, exceeds LLONG_MAX
INT_MAX = 2147483647


def resolve_dataset_path():
    """Return the dataset path from argv or fall back to the DiskGalaxy sample."""
    script_dir = os.path.abspath(os.path.dirname(__file__))
    default_dataset = os.path.join(script_dir, "DiskGalaxy", "plt0000020")
    dataset = default_dataset
    if len(sys.argv) > 1:
        dataset = sys.argv[1]
    dataset = os.path.abspath(dataset)
    if not os.path.isdir(dataset):
        raise RuntimeError(f"Dataset '{dataset}' does not exist or is not a directory")
    return dataset


def make_huge_suffix_timestep(dataset):
    """Symlink a sibling plotfile directory with a ULLONG_MAX timestep suffix."""
    parent = os.path.dirname(dataset)
    name = os.path.basename(dataset)
    prefix = name.rstrip("0123456789")
    if prefix == name:
        raise RuntimeError(f"Dataset '{name}' has no numeric timestep suffix")
    link_path = os.path.join(parent, f"{prefix}{HUGE_SUFFIX}")
    if os.path.exists(link_path):
        raise RuntimeError(f"Refusing to overwrite existing '{link_path}'")
    os.symlink(name, link_path)
    return link_path


def main():
    dataset = resolve_dataset_path()
    link_path = make_huge_suffix_timestep(dataset)
    try:
        print(f"Opening dataset: {dataset}")
        OpenDatabase(dataset, 0, "amrex-plotfile")

        nstates = TimeSliderGetNStates()
        print(f"Number of timesteps: {nstates}")
        if nstates < 2:
            print(
                "FAIL: The huge-suffix plotfile was dropped from the series; "
                "suffix parsing rejected a value that fits unsigned long long."
            )
            return 1

        metadata = GetMetaData(dataset)
        scalar_names = [
            metadata.GetScalars(i).name for i in range(metadata.GetNumScalars())
        ]
        field_scalars = [s for s in scalar_names if not s.startswith("particles/")]
        if not field_scalars:
            print("FAIL: No field scalar variables found in the dataset.")
            return 1

        GetLastError(1)
        AddPlot("Pseudocolor", field_scalars[0])
        p = PseudocolorAttributes()
        p.scaling = p.Linear
        SetPlotOptions(p)
        SetTimeSliderState(nstates - 1)
        DrawPlots()
        Query("Cycle")
        last_error = GetLastError()
        if last_error:
            print(f"FAIL: Cycle query failed -> {last_error}")
            DeleteAllPlots()
            return 1
        cycle = GetQueryOutputValue()
        print(f"Cycle at last state: {cycle}")
        DeleteAllPlots()

        if cycle < 0:
            print("FAIL: Cycle wrapped negative; unsigned-to-int cast overflowed.")
            return 1
        if int(cycle) != INT_MAX:
            print(f"FAIL: Expected cycle clamped to {INT_MAX}, got {cycle}.")
            return 1

        print("PASS: Huge timestep suffix is kept and its cycle clamps to INT_MAX.")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        DeleteAllPlots()
        os.remove(link_path)


if __name__ == "__main__":
    sys.exit(main())
