# Unreferenced helper functions contain latent bugs

- Severity: Low
- Component: `avtamrexFileFormat.C` (`ResolveDescriptorPaths`, `ParsePattern`, `ParseMeshLevel`, `BuildDomainBoundaryList`)

## Summary
Four functions are declared in `avtamrexFileFormat.h` and defined in the implementation file but are never called from anywhere in the plugin:

- `ResolveDescriptorPaths` (line 1172) and its only consumer of `ParsePattern` (line 209)
- `ParseMeshLevel` (line 2698)
- `BuildDomainBoundaryList` (line 2856)

Each carries a latent bug that will bite if the function is ever wired up:

1. **`ParsePattern`**: `std::stoi(path.substr(widthStart, ...))` (line 232) throws an uncaught `std::out_of_range` for width strings exceeding `INT_MAX` (e.g. `%099999999999T`), which would crash descriptor parsing instead of reporting a bad pattern.
2. **`ParseMeshLevel`**: `std::isdigit(meshName[digitsEnd])` (line 2708) passes a plain `char` — undefined behavior for negative (non-ASCII) characters; every other call site in the file correctly casts to `unsigned char`. `std::stoi` (line 2712) can also throw `std::out_of_range` uncaught.
3. **`BuildDomainBoundaryList`**: neighbor detection iterates over **all** patches in the hierarchy without filtering by level. Logical extents of different refinement levels live in different index spaces, so a level-0 patch and a level-1 patch whose level-local indices happen to abut are reported as face neighbors, producing bogus boundary lists for any multi-level dataset.

## Impact
- None today (dead code), but the functions present an attractive trap: they look ready to use, and `BuildDomainBoundaryList` in particular would silently produce wrong ghost communication if adopted.

## Proposed Fix
Either delete the four functions (and their declarations in `avtamrexFileFormat.h`), or fix the latent issues before first use:

- Wrap the `std::stoi` calls in `try/catch` (treat overflow as "no pattern" / level 0).
- Cast to `unsigned char` before `std::isdigit` in `ParseMeshLevel`.
- In `BuildDomainBoundaryList`, skip patches on a different level:

```diff
@@ -2890,6 +2890,10 @@ avtamrexFileFormat::BuildDomainBoundaryList(
   for (size_t otherIdx = 0; otherIdx < hierarchy.patches.size(); ++otherIdx) {
     if (static_cast<int>(otherIdx) == domain) {
       continue;
     }
+    if (hierarchy.levelIdsPerPatch[otherIdx] !=
+        hierarchy.levelIdsPerPatch[domain]) {
+      continue;
+    }
 
     const PatchInfo &other = hierarchy.patches[otherIdx];
```
