# `LoadScalarPatchData` reads misaligned data when FAB boxes include ghost cells

- Severity: Medium
- Component: `avtamrexFileFormat.C` (`LoadScalarPatchData`)

## Summary
When the on-disk FAB box differs from the patch's valid box (i.e. the FAB was written with ghost cells, so `fab.box()` is larger than `patch.cellBox`), `LoadScalarPatchData` takes the strided-copy path. That path is supposed to map patch-local indices into FAB-local indices, but the offset computation cancels itself out:

```cpp
int kk = loZ + (patch.spatialDim >= 3 ? k : 0);
const int kOffset = kk - loZ;            // == k, always
...
int jj = loY + (patch.spatialDim >= 2 ? j : 0);
const int jOffset = jj - loY;            // == j, always
const amrex::Real *row = src + (kOffset * fabNy + jOffset) * fabNx;
```

`kOffset`/`jOffset` reduce to `k`/`j`, and the row pointer never adds an x offset at all. The copy therefore always starts at the FAB's first (ghost) cell instead of at `patch.offset - fab.box().smallEnd()`, returning ghost-region values shifted into the valid region.

The fast path (`nx == fabNx && ny == fabNy && nz == fabNz`) is unaffected, which is why standard zero-ghost plotfiles render correctly today.

## Impact
- Any plotfile whose level MultiFabs are stored with `nGrow > 0` renders silently wrong data: every patch's values are shifted by the ghost width in x, y, and z.
- No error is raised, so the corruption is easy to miss.

## Reproduction
1. Write an AMReX plotfile whose MultiFab data includes ghost cells (`VisMF::Write` of a MultiFab with `nGrow > 0`).
2. Open it in VisIt with this plugin and pseudocolor any scalar.
3. Compare against `amrvis`/`yt`: values are offset by the ghost width.

## Proposed Fix
Apply the FAB-relative start offsets of the patch's valid box in all three axes.

```diff
diff --git a/avtamrexFileFormat.C b/avtamrexFileFormat.C
--- a/avtamrexFileFormat.C
+++ b/avtamrexFileFormat.C
@@ -3318,17 +3318,21 @@ vtkDataArray *avtamrexFileFormat::LoadScalarPatchData(
   if (nx == fabNx && ny == fabNy && nz == fabNz) {
     std::copy(src, src + static_cast<vtkIdType>(tupleCount), buffer);
   } else {
+    const int startX =
+        static_cast<int>(patch.offset.size() > 0 ? patch.offset[0] : loX) - loX;
+    const int startY =
+        static_cast<int>(patch.offset.size() > 1 ? patch.offset[1] : loY) - loY;
+    const int startZ =
+        static_cast<int>(patch.offset.size() > 2 ? patch.offset[2] : loZ) - loZ;
     vtkIdType idx = 0;
     for (int k = 0; k < nz; ++k) {
-      int kk = loZ + (patch.spatialDim >= 3 ? k : 0);
-      const int kOffset = kk - loZ;
+      const int kOffset = startZ + (patch.spatialDim >= 3 ? k : 0);
       for (int j = 0; j < ny; ++j) {
-        int jj = loY + (patch.spatialDim >= 2 ? j : 0);
-        const int jOffset = jj - loY;
+        const int jOffset = startY + (patch.spatialDim >= 2 ? j : 0);
         const amrex::Real *row =
-            src + (kOffset * fabNy + jOffset) * fabNx;
+            src + (static_cast<std::ptrdiff_t>(kOffset) * fabNy + jOffset) *
+                      fabNx +
+                startX;
         std::copy(row, row + nx, buffer + idx);
         idx += nx;
       }
     }
   }
```
