#!/usr/bin/env python3
# ABOUTME: VisIt CLI regression test for plotfiles whose MultiFabs are stored with ghost cells.
# ABOUTME: Asserts exact MinMax of the synthetic dataset; misaligned reads surface the ghost sentinel.
"""
VisIt CLI smoke test that opens the synthetic ghost-cell plotfile and checks
that the scalar values are read from the valid region, not the ghost region.

The dataset is produced by `make_ghost_plotfile.cpp`: interior cells hold
i + 100*j + 10000*k on a 16^3 domain (8 boxes), ghost cells hold -12345.
A reader that ignores the FAB/valid-box offset returns shifted data that
includes the sentinel, so MinMax detects the misalignment exactly.

Run with: `visit -cli -nowin -s example_data/check_ghost_cell_plotfile.py`
Optionally pass a different plotfile directory as the first argument.
"""

import os
import sys

EXPECTED_MIN = 0.0
EXPECTED_MAX = 151515.0  # 15 + 100*15 + 10000*15


def resolve_dataset_path():
    """Return the dataset path from argv or fall back to the ghost-cell sample."""
    script_dir = os.path.abspath(os.path.dirname(__file__))
    default_dataset = os.path.join(script_dir, "GhostCells", "plt_ghost00000")
    dataset = default_dataset
    if len(sys.argv) > 1:
        dataset = sys.argv[1]
    dataset = os.path.abspath(dataset)
    if not os.path.isdir(dataset):
        raise RuntimeError(f"Dataset '{dataset}' does not exist or is not a directory")
    return dataset


def main():
    dataset = resolve_dataset_path()
    print(f"Opening dataset: {dataset}")
    OpenDatabase(dataset, 0, "amrex-plotfile")

    GetLastError(1)
    AddPlot("Pseudocolor", "testvar")
    p = PseudocolorAttributes()
    p.scaling = p.Linear
    SetPlotOptions(p)
    DrawPlots()
    Query("MinMax")
    last_error = GetLastError()
    if last_error:
        print(f"FAIL: MinMax query failed -> {last_error}")
        DeleteAllPlots()
        return 1

    result = GetQueryOutputValue()
    vmin, vmax = float(result[0]), float(result[1])
    print(f"MinMax testvar -> ({vmin}, {vmax})")

    if vmin != EXPECTED_MIN or vmax != EXPECTED_MAX:
        print(
            f"FAIL: expected ({EXPECTED_MIN}, {EXPECTED_MAX}); "
            "ghost-cell FAB data was read misaligned."
        )
        DeleteAllPlots()
        return 1

    print("PASS: ghost-cell plotfile values are read from the valid region.")
    DeleteAllPlots()
    return 0


if __name__ == "__main__":
    sys.exit(main())
