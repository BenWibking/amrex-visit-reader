# Cycle numbers can overflow and wrap in `GetCycles`

- Severity: Low
- Status: Fixed — values above INT_MAX clamp to INT_MAX with a debug1
  warning. Regression-tested by `smoke_test_large_cycles.sh`: a symlinked
  timestep named `plt18446744073709551615` reports cycle 2147483647.
- Component: `avtamrexFileFormat.C` (`GetCycles`)

## Summary
`iterationIndex_` is stored as `unsigned long long`, but `GetCycles` casts each value directly to `int`:

```cpp
cycles[i] = static_cast<int>(iterationIndex_[i]);
```

For large timestep numbers (`> INT_MAX`), this overflows and can produce negative or otherwise invalid cycle values.

## Impact
- Incorrect cycle labels in VisIt UI.
- Nondeterministic behavior for time navigation when large cycle IDs are used.

## Proposed Fix
Clamp to `INT_MAX` (or another chosen sentinel) and emit a debug warning.

```diff
diff --git a/avtamrexFileFormat.C b/avtamrexFileFormat.C
index e79ee2a..6bad402 100644
--- a/avtamrexFileFormat.C
+++ b/avtamrexFileFormat.C
@@ -2429,8 +2429,17 @@ void avtamrexFileFormat::GetCycles(std::vector<int> &cycles) {
 void avtamrexFileFormat::GetCycles(std::vector<int> &cycles) {
   cycles.resize(iterationIndex_.size());
+  const auto cycleMax =
+      static_cast<unsigned long long>(std::numeric_limits<int>::max());
   for (size_t i = 0; i < iterationIndex_.size(); ++i) {
-    cycles[i] = static_cast<int>(iterationIndex_[i]);
+    const auto iter = iterationIndex_[i];
+    if (iter > cycleMax) {
+      debug1 << "[amrex-plugin] Cycle value " << iter
+             << " exceeds INT_MAX; clamping to "
+             << std::numeric_limits<int>::max() << "\n";
+      cycles[i] = std::numeric_limits<int>::max();
+    } else {
+      cycles[i] = static_cast<int>(iter);
+    }
   }
 }
```
