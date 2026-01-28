#!/usr/bin/env python3
"""
VisIt CLI smoke test that opens an AMReX plotfile on a parallel engine.

Run with: `visit -cli -s example_data/check_parallel_plotfile_open.py`
Optionally pass a different plotfile directory as the first argument.
"""

import os
import sys
import socket


def resolve_dataset_path():
    """Return the dataset path from argv or fall back to the particle sample."""
    script_dir = os.path.abspath(os.path.dirname(__file__))
    default_dataset = os.path.join(script_dir, "StarParticles", "plrd01000")
    dataset = default_dataset
    if len(sys.argv) > 1:
        dataset = sys.argv[1]
    dataset = os.path.abspath(dataset)
    if not os.path.isdir(dataset):
        raise RuntimeError(f"Dataset '{dataset}' does not exist or is not a directory")
    return dataset


def ensure_parallel_engine():
    """
    Ensure a parallel compute engine is running.
    Returns True if a parallel engine is active (>=2 ranks).
    """
    engines = GetEngineList()
    if len(engines) > 0:
        props = GetEngineProperties(engines[0])
        if props.numProcessors >= 2:
            return True
        CloseComputeEngine()

    # Attempt to launch a parallel engine.
    hostname = socket.gethostname()
    if "dane" in hostname or "rzwhippet" in hostname:
        opened = OpenComputeEngine("localhost", ("-l", "srun", "-np", "2"))
    else:
        opened = OpenComputeEngine("localhost", ("-np", "2"))

    if not opened:
        return False

    engines = GetEngineList()
    if len(engines) == 0:
        return False

    props = GetEngineProperties(engines[0])
    return props.numProcessors >= 2


def pick_scalar_variable(dataset):
    """Pick a representative scalar variable to plot."""
    metadata = GetMetaData(dataset)
    scalar_names = [
        metadata.GetScalars(i).name for i in range(metadata.GetNumScalars())
    ]
    if not scalar_names:
        raise RuntimeError("No scalar variables found in the dataset.")
    if "gasDensity" in scalar_names:
        return "gasDensity"
    return scalar_names[0]


def main():
    dataset = resolve_dataset_path()
    print(f"Opening dataset: {dataset}")

    if not ensure_parallel_engine():
        print("FAIL: Unable to start a parallel compute engine.")
        return

    engines = GetEngineList()
    props = GetEngineProperties(engines[0])
    print(f"Parallel engine ranks: {props.numProcessors}")

    OpenDatabase(dataset, 0, "amrex-plotfile")
    scalar_var = pick_scalar_variable(dataset)
    print(f"Using scalar variable: {scalar_var}")

    AddPlot("Pseudocolor", scalar_var)
    DrawPlots()

    Query("NumZones")
    zones = GetQueryOutputValue()
    if isinstance(zones, (list, tuple)):
        zones = zones[0]
    zones = float(zones)
    print(f"Zone count reported: {zones}")

    if zones > 0 and props.numProcessors >= 2:
        print("PASS: Plotfile opened and queried on a parallel engine.")
    else:
        print("FAIL: Plotfile open/query did not succeed on parallel engine.")

    DeleteAllPlots()


if __name__ == "__main__":
    main()
