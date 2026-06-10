# `ParsePlotfileDirectoryName` parses unsigned timestep suffixes with signed `std::stoll`

- Severity: Low
- Component: `avtamrexFileFormat.C` (`ParsePlotfileDirectoryName`, also `ResolveDescriptorPaths`)

## Summary
`ParsePlotfileDirectoryName` extracts the trailing digits of a plotfile directory name into an `unsigned long long`, but parses them with the signed `std::stoll`:

```cpp
iteration = static_cast<unsigned long long>(std::stoll(digits));
```

For digit strings in `(LLONG_MAX, ULLONG_MAX]` — values that fit the declared `unsigned long long` type — `std::stoll` throws `std::out_of_range`, the catch returns `false`, and the directory is treated as *not* a plotfile. The same pattern appears in `ResolveDescriptorPaths` (line 1266), though that function is currently unreferenced.

## Impact
- A plotfile whose timestep suffix exceeds `LLONG_MAX` (19–20 digits) is silently excluded from the time series; if it is the directory the user opened, the constructor fails with "No plotfile directories matching ... found".
- Pathological in practice (AMReX step counters rarely approach 2^63), hence Low severity — but the type was clearly chosen to hold the full unsigned range, and the parser quietly doesn't.

## Reproduction
1. Create `plt9300000000000000000/` (valid plotfile contents, suffix > `LLONG_MAX`).
2. Open it in VisIt.
3. The constructor's directory scan rejects the name and throws `InvalidFilesException`.

## Proposed Fix
Use the unsigned parser.

```diff
diff --git a/avtamrexFileFormat.C b/avtamrexFileFormat.C
--- a/avtamrexFileFormat.C
+++ b/avtamrexFileFormat.C
@@ -1112,7 +1112,7 @@ inline bool ParsePlotfileDirectoryName(const std::string &name,
   prefix = name.substr(0, start);
   std::string digits = name.substr(start);
   try {
-    iteration = static_cast<unsigned long long>(std::stoll(digits));
+    iteration = std::stoull(digits);
   } catch (std::exception const &) {
     return false;
   }
```

(Apply the same change to `ResolveDescriptorPaths` if that helper is ever wired up; see issue 010.)
