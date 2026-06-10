# `check_inverse_ghost_zones.py` reports failure text but exits with success

- Severity: Medium
- Status: Fixed — `main()` returns 0/1 (including on query failure) and is
  invoked via `sys.exit(main())`. Verified: PASS run exits 0.
- Component: `example_data/check_inverse_ghost_zones.py`

## Summary
The script prints `FAIL: ...` when no valid zones remain after `InverseGhostZone`, but it never returns a non-zero exit status.

`main()` does not return a code, and the module calls `main()` directly (not `sys.exit(main())`). As a result, CI/smoke wrappers treat failures as success.

## Impact
- Regression checks can silently pass when ghost synthesis is broken.
- Automation cannot rely on process exit code for pass/fail.

## Proposed Fix
Return explicit status codes from `main()` and propagate via `sys.exit(main())`.

```diff
diff --git a/example_data/check_inverse_ghost_zones.py b/example_data/check_inverse_ghost_zones.py
index 8f83f57..7de568f 100755
--- a/example_data/check_inverse_ghost_zones.py
+++ b/example_data/check_inverse_ghost_zones.py
@@ -8,6 +8,7 @@ Optionally pass a different plotfile directory as the first argument.
 
 import os
 import sys
+
 
 def resolve_dataset_path():
@@ -65,13 +66,16 @@ def main():
 
     if remaining_zones > 0:
         print("PASS: Valid zones remain, indicating ghosts were created.")
+        exit_code = 0
     else:
         print(
             "FAIL: No valid zones remain after Inverse Ghost Zone; "
             "ghost generation likely missing."
         )
+        exit_code = 1
 
     DeleteAllPlots()
+    return exit_code
 
 
 if __name__ == "__main__":
-    main()
+    sys.exit(main())
```
