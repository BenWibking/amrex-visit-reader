#!/usr/bin/env python3
"""
VisIt CLI smoke test that clears engine caches and re-queries particle vars.

Run with: `visit -cli -s example_data/check_particle_plotfile_clearcache.py`
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


def pick_first_var(metadata, group, mesh_name):
    vars_for = vars_for_mesh(metadata, group, mesh_name)
    return vars_for[0] if vars_for else None


def query_minmax(plot_type, var_name):
    GetLastError(1)
    AddPlot(plot_type, var_name)
    DrawPlots()
    Query("MinMax")
    last_error = GetLastError()
    if last_error:
        raise RuntimeError(last_error)
    value = None
    if "GetQueryOutputValue" in globals():
        value = GetQueryOutputValue()
    if value is None:
        value = GetQueryOutputString()
    DeleteActivePlots()
    return value


def query_num_nodes(mesh_name):
    GetLastError(1)
    AddPlot("Mesh", mesh_name)
    DrawPlots()
    Query("NumNodes")
    last_error = GetLastError()
    if last_error:
        raise RuntimeError(last_error)
    value = None
    if "GetQueryOutputValue" in globals():
        value = GetQueryOutputValue()
    if value is None:
        value = GetQueryOutputString()
    DeleteActivePlots()
    return value


def define_vector_component_expr(base_name, comp_index, defined):
    expr_name = f"{base_name.replace('/', '_').replace(':', '_').replace('.', '_')}_c{comp_index}"
    if expr_name in defined:
        return expr_name
    expr_def = f"<{base_name}>[{comp_index}]"
    try:
        DefineScalarExpression(expr_name, expr_def)
    except TypeError:
        DefineScalarExpression(expr_name, expr_def, 0, 0)
    except Exception as exc:
        print(f"WARN: Failed to define expression {expr_name} -> {exc}")
        return None
    defined.add(expr_name)
    return expr_name


def main():
    dataset = resolve_dataset_path()
    print(f"Opening dataset: {dataset}")

    if not ensure_parallel_engine():
        print("FAIL: Unable to start a parallel compute engine.")
        return 1

    OpenDatabase(dataset, 0, "amrex-plotfile")
    metadata = GetMetaData(dataset)
    check_vectors = os.environ.get("CHECK_PARTICLE_VECTORS", "0") == "1"

    point_meshes = find_point_meshes(metadata)
    if not point_meshes:
        print("FAIL: No point meshes found in the dataset metadata.")
        return 1

    defined_expressions = set()
    for mesh in point_meshes:
        mesh_name = mesh.name
        scalar_vars = vars_for_mesh(metadata, "Scalars", mesh_name)
        vector_vars = vars_for_mesh(metadata, "Vectors", mesh_name)

        if not scalar_vars and not vector_vars:
            print(f"FAIL: No per-particle variables found for {mesh_name}.")
            DeleteAllPlots()
            return 1

        scalar_var = scalar_vars[0] if scalar_vars else None
        vector_var = vector_vars[0] if vector_vars else None
        print(f"Particle mesh: {mesh_name}")
        print(f"Scalar var: {scalar_var}")
        print(f"Vector var: {vector_var}")

        try:
            num_nodes = query_num_nodes(mesh_name)
            print(f"NumNodes: {num_nodes}")
        except Exception as exc:
            print(f"FAIL: Mesh query failed -> {exc}")
            DeleteAllPlots()
            return 1

        try:
            if float(num_nodes) == 0:
                print("WARN: Particle mesh has zero points; skipping variable checks.")
                DeleteAllPlots()
                continue
        except Exception:
            pass

        try:
            if scalar_var:
                result = query_minmax("Pseudocolor", scalar_var)
                print(f"OK: MinMax {scalar_var} -> {result}")
            if vector_var and check_vectors:
                expr_name = define_vector_component_expr(
                    vector_var, 0, defined_expressions)
                if expr_name:
                    try:
                        result = query_minmax("Pseudocolor", expr_name)
                        print(f"OK: MinMax {vector_var}[0] -> {result}")
                    except Exception as exc:
                        print(f"WARN: Vector component query failed -> {exc}")
        except Exception as exc:
            print(f"FAIL: Initial particle query failed -> {exc}")
            DeleteAllPlots()
            return 1

        print("Clearing engine caches...")
        ClearCacheForAllEngines()
        DeleteAllPlots()
        if "ReOpenDatabase" in globals():
            ReOpenDatabase(dataset)
        else:
            OpenDatabase(dataset, 0, "amrex-plotfile")
        metadata = GetMetaData(dataset)
        scalar_var = pick_first_var(metadata, "Scalars", mesh_name)
        vector_var = pick_first_var(metadata, "Vectors", mesh_name)

        try:
            if scalar_var:
                result = query_minmax("Pseudocolor", scalar_var)
                print(f"OK: Post-clear MinMax {scalar_var} -> {result}")
            if vector_var and check_vectors:
                expr_name = define_vector_component_expr(
                    vector_var, 0, defined_expressions)
                if expr_name:
                    result = query_minmax("Pseudocolor", expr_name)
                    print(f"OK: Post-clear MinMax {vector_var}[0] -> {result}")
        except Exception as exc:
            print(f"FAIL: Post-clear particle query failed -> {exc}")
            DeleteAllPlots()
            return 1

    print("PASS: Particle variables still render after ClearCache.")
    DeleteAllPlots()
    try:
        CloseDatabase(dataset)
    except Exception:
        pass
    try:
        CloseComputeEngine()
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
