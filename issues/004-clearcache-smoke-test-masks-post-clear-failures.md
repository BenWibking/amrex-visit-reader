# `check_particle_plotfile_clearcache.py` masks post-clear regressions as warnings

- Severity: High
- Status: Fixed — post-clear scalar/vector query failures now propagate to the outer handler, which prints `FAIL` and returns 1. Verified: `smoke_test_particles_clearcache.sh` still passes.
- Component: `example_data/check_particle_plotfile_clearcache.py`

## Summary
The script is intended to verify that particle variables still render *after* `ClearCacheForAllEngines()`. However, post-clear query failures are caught and downgraded to `WARN`, and the script still returns success.

This suppresses exactly the regression the script is meant to catch.

## Impact
- Cache-clear regressions can pass smoke tests unnoticed.
- False confidence in plugin stability after cache invalidation.

## Proposed Fix
Treat post-clear scalar/vector query failures as hard failures and return non-zero.

```diff
diff --git a/example_data/check_particle_plotfile_clearcache.py b/example_data/check_particle_plotfile_clearcache.py
index 363e696..b262a89 100755
--- a/example_data/check_particle_plotfile_clearcache.py
+++ b/example_data/check_particle_plotfile_clearcache.py
@@ -215,20 +215,14 @@ def main():
         try:
             if scalar_var:
-                try:
-                    result = query_minmax("Pseudocolor", scalar_var)
-                    print(f"OK: Post-clear MinMax {scalar_var} -> {result}")
-                except Exception as exc:
-                    print(f"WARN: Post-clear scalar query failed -> {exc}")
+                result = query_minmax("Pseudocolor", scalar_var)
+                print(f"OK: Post-clear MinMax {scalar_var} -> {result}")
             if vector_var and check_vectors:
                 expr_name = define_vector_component_expr(
                     vector_var, 0, defined_expressions)
                 if expr_name:
-                    try:
-                        result = query_minmax("Pseudocolor", expr_name)
-                        print(f"OK: Post-clear MinMax {vector_var}[0] -> {result}")
-                    except Exception as exc:
-                        print(f"WARN: Post-clear vector component query failed -> {exc}")
+                    result = query_minmax("Pseudocolor", expr_name)
+                    print(f"OK: Post-clear MinMax {vector_var}[0] -> {result}")
         except Exception as exc:
             print(f"FAIL: Post-clear particle query failed -> {exc}")
             DeleteAllPlots()
```
