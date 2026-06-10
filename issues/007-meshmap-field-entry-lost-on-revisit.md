# `PopulateDatabaseMetaData` can leave the field mesh unregistered in `meshMap_` (defensive hardening)

- Severity: Low (originally rated High; downgraded after verification — see below)
- Status: Fixed (`BuildFieldHierarchy` now re-registers the field mesh)
- Component: `avtamrexFileFormat.C` (`PopulateDatabaseMetaData`, `BuildFieldHierarchy`, `PopulateHierarchyCache`)

## Summary
`PopulateDatabaseMetaData` clears `meshMap_` on every call (line 2406), but the field mesh's `meshMap_` entry was only registered inside `PopulateHierarchyCache`, which `EnsureHierarchyInitialized` skips when `meshHierarchyCache_[timeState]` is already populated (line 2456). `BuildParticleHierarchy` re-registers particle meshes itself, so only the field mesh entry could be lost.

`GetMesh` requires a `meshMap_` hit (line 3486) and throws `InvalidVariableException` when it is missing — there is no fallback.

If `PopulateDatabaseMetaData` ever runs for a timestep whose hierarchy cache is warm, all subsequent field-mesh `GetMesh` calls fail until something repopulates the cache.

## Verification (2026-06-10)
A regression test was added (`example_data/check_field_timestep_revisit.py`, wrapper `smoke_test_field_revisit.sh`) that symlinks a second timestep, plots a field scalar at state 0 → 1 → back to 0. Run against a build **without** the fix, the test still passed. Engine debug logs (`-debug 1`) show why:

- The engine calls `PopulateDatabaseMetaData` only on the *first* visit to each timestep (once for state 0, once for state 1, and not again on revisit).
- `MakeDefaultMeshName` strips the timestep suffix, so the field mesh name is identical across timesteps; every cold-cache populate re-registers the shared `meshMap_` entry.

In VisIt 3.5.0's observed call flow, every `PopulateDatabaseMetaData` call coincides with a cold hierarchy cache for that timestep, so the broken window is not reachable. `FreeUpResources` (e.g. `ClearCacheForAllEngines`) resets both caches consistently and also avoids it. The original High rating assumed metadata is re-populated on every timestep activation (`HasInvariantMetaData() == false`); that turned out not to hold for the engine.

The fix is retained as cheap hardening: the invariant "the field mesh is registered whenever metadata maps are rebuilt" no longer depends on cache state or on which VisIt version's call ordering is in effect.

## Fix
Applied in `BuildFieldHierarchy`, which runs on every `PopulateDatabaseMetaData` call regardless of cache state:

```diff
diff --git a/avtamrexFileFormat.C b/avtamrexFileFormat.C
--- a/avtamrexFileFormat.C
+++ b/avtamrexFileFormat.C
@@ -1996,6 +1996,11 @@ void avtamrexFileFormat::BuildFieldHierarchy(avtDatabaseMetaData *md,
     EXCEPTION1(InvalidVariableException, meshName.c_str());
   }
 
   const MeshPatchHierarchy &hierarchy = it->second;
+  // PopulateDatabaseMetaData clears meshMap_ on every call, but
+  // PopulateHierarchyCache only runs when the hierarchy cache is empty.
+  // Re-register the field mesh here so the meshMap_ entry exists whenever
+  // metadata is populated, regardless of hierarchy cache state.
+  meshMap_[meshName] = std::tuple(DatasetType::Field, meshName);
 
   if (md != nullptr) {
```
