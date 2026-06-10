# `GetMesh` can reject valid particle meshes before lazy particle initialization

- Severity: Medium
- Status: Fixed — `GetMesh` now retries the `meshMap_` lookup after
  `EnsureParticleHierarchyInitialized(timeState)` when the requested name
  starts with `particles/`. The lazy ordering is hard to trigger from the
  CLI, so verification is build + full smoke suite (no regressions).
- Component: `avtamrexFileFormat.C` (`GetMesh`)

## Summary
`GetMesh` performs `meshMap_.find(visit_meshname)` before attempting particle hierarchy initialization. Particle meshes are registered in `meshMap_` only when particle hierarchy is built, so a direct `GetMesh("particles/<species>")` can fail depending on call ordering.

## Impact
- Behavior depends on whether metadata/particle hierarchy was built earlier in the same timestep.
- Valid particle mesh requests can throw `InvalidVariableException` in lazy paths.
- Makes reader behavior nondeterministic across workflow orderings.

## Reproduction
1. Open a dataset with particle species.
2. Trigger `GetMesh` for `particles/<species>` before a metadata path that builds particles.
3. `meshMap_` lookup misses and `GetMesh` throws.
4. If metadata is populated first, the same request succeeds.

## Proposed Fix
If initial lookup misses and the requested name is a particle mesh, initialize particle hierarchy for that timestep and retry the lookup.

```diff
diff --git a/avtamrexFileFormat.C b/avtamrexFileFormat.C
index e79ee2a..e3041ac 100644
--- a/avtamrexFileFormat.C
+++ b/avtamrexFileFormat.C
@@ -3483,7 +3483,18 @@ vtkDataSet *avtamrexFileFormat::GetMesh(int timeState, int domain,
 
   EnsureHierarchyInitialized(timeState);
 
-  auto meshTypeIt = meshMap_.find(visit_meshname);
+  auto meshTypeIt = meshMap_.find(visit_meshname);
+  if (meshTypeIt == meshMap_.end()) {
+    // Particle mesh names are registered during particle hierarchy build.
+    // Retry after lazy initialization so GetMesh does not depend on
+    // metadata-population ordering.
+    if (std::strncmp(visit_meshname, "particles/", 10) == 0) {
+      EnsureParticleHierarchyInitialized(timeState);
+      meshTypeIt = meshMap_.find(visit_meshname);
+    }
+  }
+
   if (meshTypeIt == meshMap_.end()) {
     debug1 << "[amrex-plugin] GetMesh missing mesh '" << meshName << "'\n";
     EXCEPTION1(InvalidVariableException, visit_meshname);
```

## Notes
This change preserves existing behavior for field meshes and only expands lazy initialization for particle mesh names.
