#!/usr/bin/env python3
# ABOUTME: VisIt CLI regression test that re-plots a field variable after revisiting a timestep.
# ABOUTME: Guards against metadata re-population dropping the field mesh registration (issue 007).
"""
VisIt CLI smoke test that plots a field scalar, steps to another timestep,
returns to the first timestep, and plots again.

The plugin reader rebuilds its variable maps every time VisIt re-reads
metadata for a timestep; this exercises the path where the per-timestep
hierarchy cache is already warm. The test symlinks a sibling plotfile
directory so the series has two timesteps, and removes the symlink on exit.

Run with: `visit -cli -nowin -s example_data/check_field_timestep_revisit.py`
Optionally pass a different plotfile directory as the first argument.
"""

import os
import sys


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


def make_second_timestep(dataset):
    """Symlink a sibling plotfile directory so the series has two timesteps."""
    parent = os.path.dirname(dataset)
    name = os.path.basename(dataset)
    prefix = name.rstrip("0123456789")
    digits = name[len(prefix):]
    if not digits:
        raise RuntimeError(f"Dataset '{name}' has no numeric timestep suffix")
    next_name = f"{prefix}{int(digits) + 1:0{len(digits)}d}"
    link_path = os.path.join(parent, next_name)
    if os.path.exists(link_path):
        raise RuntimeError(f"Refusing to overwrite existing '{link_path}'")
    os.symlink(name, link_path)
    return link_path


def query_minmax(var_name):
    """Plot the variable, query MinMax, and return the result."""
    GetLastError(1)
    AddPlot("Pseudocolor", var_name)
    DrawPlots()
    Query("MinMax")
    last_error = GetLastError()
    if last_error:
        raise RuntimeError(last_error)
    value = GetQueryOutputValue()
    DeleteActivePlots()
    return value


def main():
    dataset = resolve_dataset_path()
    link_path = make_second_timestep(dataset)
    try:
        print(f"Opening dataset: {dataset}")
        OpenDatabase(dataset, 0, "amrex-plotfile")
        metadata = GetMetaData(dataset)
        scalar_names = [
            metadata.GetScalars(i).name for i in range(metadata.GetNumScalars())
        ]
        field_scalars = [s for s in scalar_names if not s.startswith("particles/")]
        if not field_scalars:
            print("FAIL: No field scalar variables found in the dataset.")
            return 1
        var = field_scalars[0]
        print(f"Field scalar: {var}")

        first = query_minmax(var)
        print(f"OK: MinMax {var} at state 0 -> {first}")

        SetTimeSliderState(1)
        second = query_minmax(var)
        print(f"OK: MinMax {var} at state 1 -> {second}")

        SetTimeSliderState(0)
        revisit = query_minmax(var)
        print(f"OK: MinMax {var} at revisited state 0 -> {revisit}")

        if revisit != first:
            print("FAIL: Revisited state 0 returned different values.")
            return 1

        print("PASS: Field variable renders after revisiting a timestep.")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        DeleteAllPlots()
        os.remove(link_path)


if __name__ == "__main__":
    sys.exit(main())
