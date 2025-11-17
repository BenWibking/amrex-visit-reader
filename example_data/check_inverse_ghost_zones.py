#!/usr/bin/env python3
"""
VisIt CLI helper that loads an example AMReX dataset, applies the
Inverse Ghost Zone filter, and verifies that valid zones remain.

Run with: `visit -cli -s example_data/check_inverse_ghost_zones.py`
Optionally pass a different .amrex file as the first argument.
"""

import os
import sys


def resolve_dataset_path():
    """Return the dataset path from argv or fall back to the default 3D sample."""
    script_dir = os.path.abspath(os.path.dirname(__file__))
    default_dataset = os.path.join(script_dir, "hdf5_3d.amrex")
    dataset = default_dataset
    if len(sys.argv) > 1:
        dataset = sys.argv[1]
    dataset = os.path.abspath(dataset)
    if not os.path.exists(dataset):
        raise RuntimeError(f"Dataset '{dataset}' does not exist")
    return dataset


def add_scalar_plot(var_name):
    """
    Adds a Pseudocolor plot of the requested variable.
    This keeps the script agnostic to specific View attributes.
    """
    AddPlot("Pseudocolor", var_name)
    DrawPlots()


def query_value(query_name, variable=None):
    """Execute a VisIt query and return the numeric result."""
    try:
        if variable is None:
            Query(query_name)
        else:
            Query(query_name, variable)
    except RuntimeError as exc:
        raise RuntimeError(
            f"Query '{query_name}' failed; ensure the active plot is valid."
        ) from exc
    result = GetQueryOutputValue()
    if isinstance(result, (list, tuple)):
        result = result[0]
    return float(result)


def main():
    dataset = resolve_dataset_path()
    print(f"Opening dataset: {dataset}")
    OpenDatabase(dataset)

    # Choose a representative scalar variable. The default example contains 'gasDensity'.
    scalar_var = "gasDensity"
    metadata = GetMetaData(dataset)
    scalar_names = [
        metadata.GetScalars(i).name for i in range(metadata.GetNumScalars())
    ]
    if scalar_var not in scalar_names and scalar_names:
        scalar_var = scalar_names[0]
        print(f"'gasDensity' not found; using '{scalar_var}' instead.")

    add_scalar_plot(scalar_var)

    AddOperator("InverseGhostZone")
    DrawPlots()

    remaining_zones = query_value("NumZones")
    print(f"Valid zones remaining after Inverse Ghost Zone: {remaining_zones}")

    if remaining_zones > 0:
        print("PASS: Valid zones remain, indicating ghosts were created.")
    else:
        print(
            "FAIL: No valid zones remain after Inverse Ghost Zone; "
            "ghost generation likely missing."
        )

    DeleteAllPlots()


if __name__ == "__main__":
    main()
