#!/usr/bin/env python3
"""
VisIt CLI smoke test that opens an AMReX particle plotfile and renders point meshes.

Run with: `visit -cli -s example_data/check_particle_plotfile_open.py`
Optionally pass a different plotfile directory as the first argument.
"""

import os
import sys
import socket


def resolve_dataset_path():
    """Return the dataset path from argv or fall back to the particle sample."""
    script_dir = os.path.abspath(os.path.dirname(__file__))
    default_dataset = os.path.join(script_dir, "Nyx_LyA", "plt00000")
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


def iter_metadata_items(metadata, group):
    count = getattr(metadata, f"GetNum{group}")()
    getter = getattr(metadata, f"Get{group}")
    for idx in range(count):
        yield getter(idx)


def get_mesh_type(mesh_meta):
    if hasattr(mesh_meta, "meshType"):
        return mesh_meta.meshType
    if hasattr(mesh_meta, "type"):
        return mesh_meta.type
    return None


def get_mesh_name(meta):
    for attr in ("meshName", "mesh", "meshname"):
        if hasattr(meta, attr):
            return getattr(meta, attr)
    return None


def find_point_meshes(metadata):
    point_mesh_type = globals().get("AVT_POINT_MESH", 3)
    meshes = []
    for mesh in iter_metadata_items(metadata, "Meshes"):
        mesh_type = get_mesh_type(mesh)
        if mesh_type == point_mesh_type:
            meshes.append(mesh)
    return meshes


def vars_for_mesh(metadata, group, mesh_name):
    matches = []
    for var_meta in iter_metadata_items(metadata, group):
        if get_mesh_name(var_meta) == mesh_name:
            matches.append(var_meta.name)
    return matches


def render_plot(plot_type, var_name):
    GetLastError(1)
    AddPlot(plot_type, var_name)
    DrawPlots()
    query_result = None
    if plot_type == "Mesh":
        GetLastError(1)
        Query("NumNodes")
        last_error = GetLastError()
        if last_error:
            raise RuntimeError(last_error)
        value = None
        if "GetQueryOutputValue" in globals():
            value = GetQueryOutputValue()
        if value is None:
            value = GetQueryOutputString()
        query_result = f"NumNodes={value}"
    else:
        Query("MinMax")
    last_error = GetLastError()
    if last_error:
        raise RuntimeError(last_error)
    DeleteActivePlots()
    return query_result


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
    metadata = GetMetaData(dataset)

    point_meshes = find_point_meshes(metadata)
    if not point_meshes:
        print("FAIL: No point meshes found in the dataset metadata.")
        return

    failures = []
    for mesh in point_meshes:
        mesh_name = mesh.name
        scalar_vars = vars_for_mesh(metadata, "Scalars", mesh_name)
        vector_vars = vars_for_mesh(metadata, "Vectors", mesh_name)
        print(f"Point mesh: {mesh_name}")
        print(f"  Scalars: {scalar_vars}")
        print(f"  Vectors: {vector_vars}")

        try:
            mesh_count = render_plot("Mesh", mesh_name)
            if mesh_count:
                print(f"OK: Mesh {mesh_name} (NumPoints: {mesh_count})")
            else:
                print(f"OK: Mesh {mesh_name}")
        except Exception as exc:
            failures.append((mesh_name, f"mesh render failed: {exc}"))
            print(f"FAIL: Mesh {mesh_name} -> {exc}")
            DeleteActivePlots()
            continue

        plot_var = None
        if scalar_vars:
            plot_var = scalar_vars[0]
            plot_type = "Pseudocolor"
        elif vector_vars:
            plot_var = vector_vars[0]
            plot_type = "Vector"

        if plot_var is None:
            print(f"WARN: No per-particle variables found for {mesh_name}.")
            continue

        try:
            render_plot(plot_type, plot_var)
            print(f"OK: {plot_type} {plot_var}")
        except Exception as exc:
            failures.append((plot_var, str(exc)))
            print(f"FAIL: {plot_type} {plot_var} -> {exc}")
            DeleteActivePlots()

    if failures:
        print("FAILED PARTICLE PLOTS:")
        for name, msg in failures:
            print(f"  {name}: {msg}")
        print("FAIL: One or more particle plots failed to render.")
    else:
        print("PASS: Particle meshes rendered successfully.")

    DeleteAllPlots()


if __name__ == "__main__":
    main()
