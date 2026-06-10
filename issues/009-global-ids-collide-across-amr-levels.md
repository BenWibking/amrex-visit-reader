# Global zone/node IDs collide across AMR levels

- Severity: Low
- Component: `avtamrexFileFormat.C` (`BuildGlobalZoneIds`, `BuildGlobalNodeIds`)

## Summary
`BuildGlobalZoneIds` and `BuildGlobalNodeIds` compute a global ID as `gi + strideY*gj + strideZ*gk` from each patch's logical (level-local) indices. The strides come from `ComputeGlobalCellDimensions`/`ComputeGlobalNodeDimensions`, which take the maximum logical extent over **all patches of all levels**.

Because logical indices of different refinement levels live in different index spaces, a level-0 cell and a level-1 cell with the same `(gi, gj, gk)` tuple receive the **same** "global" ID. The IDs are therefore not globally unique whenever the hierarchy has more than one level.

A secondary effect: the strides are inflated by the finest level's extents, so coarse-level IDs are sparse and inconsistent with the coarse level's own index space.

## Impact
- VisIt uses global node/zone IDs (when a plugin offers them via `GetAuxiliaryData`) to identify coincident entities across domains. Colliding IDs across levels can cause incorrect merging/deduplication in pipelines that request `AUXILIARY_DATA_GLOBAL_NODE_IDS` / `AUXILIARY_DATA_GLOBAL_ZONE_IDS`.
- Only affects multi-level datasets and only operations that consume these arrays, hence Low severity.

## Reproduction
1. Open a 2-level plotfile.
2. Request global zone IDs for a level-0 domain and a level-1 domain whose logical lower corners coincide (e.g. both contain `(0,0,0)`).
3. Both domains report ID 0 for different physical cells.

## Proposed Fix
Make IDs unique by computing per-level dimensions and adding a per-level base offset (cumulative size of all coarser levels). Sketch for the zone variant (node variant analogous):

```diff
diff --git a/avtamrexFileFormat.C b/avtamrexFileFormat.C
--- a/avtamrexFileFormat.C
+++ b/avtamrexFileFormat.C
@@ -3031,9 +3031,28 @@ avtamrexFileFormat::BuildGlobalZoneIds(const MeshPatchHierarchy &hierarchy,
   bool meshNodeCentered = MeshIsNodeCentered(hierarchy);
-  std::array<int, 3> globalDims =
-      ComputeGlobalCellDimensions(hierarchy, meshNodeCentered);
   const PatchInfo &patch = hierarchy.patches[domain];
+  // Compute dimensions from the patch's own level only, and offset IDs by
+  // the total cell count of all coarser levels so IDs are globally unique.
+  const int patchLevel = hierarchy.levelIdsPerPatch[domain];
+  std::array<int, 3> globalDims{1, 1, 1};
+  vtkIdType levelBase = 0;
+  for (int level = 0; level <= patchLevel; ++level) {
+    std::array<int, 3> dims{1, 1, 1};
+    for (int patchIdx : hierarchy.patchesPerLevel[level]) {
+      const PatchInfo &p = hierarchy.patches[patchIdx];
+      for (int axis = 0; axis < hierarchy.topologicalDim; ++axis) {
+        dims[axis] = std::max(dims[axis], p.logicalUpper[axis] + 1);
+      }
+    }
+    if (level < patchLevel) {
+      levelBase += static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2];
+    } else {
+      globalDims = dims;
+    }
+  }
   std::array<int, 3> counts =
       ComputePatchCellCounts(patch, hierarchy.topologicalDim, meshNodeCentered);
```

and add `levelBase` to each computed `globalId`. (The `meshNodeCentered` adjustments from `ComputeGlobalCellDimensions` should be folded into the per-level loop the same way they are today.)
