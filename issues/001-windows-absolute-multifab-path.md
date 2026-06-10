# Windows absolute MultiFab paths are treated as relative

- Severity: High
- Status: Fixed — both call sites now use `IsAbsolutePath(...)`/`JoinPath(...)`. POSIX behavior is unchanged (verified via the smoke tests); the Windows path classes are handled by the existing cross-platform helpers. Not runtime-tested on Windows.
- Component: `avtamrexFileFormat.C` (`GetVisMF`, `GetMultiFabName`)

## Summary
The reader decides whether a MultiFab path is absolute with `mfName[0] != '/'`. That works on POSIX but fails on Windows absolute paths like `C:\\...` and `\\server\\share\\...`.

When this happens, the code prepends the plotfile directory and produces an invalid path.

## Impact
- Valid AMReX plotfiles fail to load on Windows.
- Failures occur during scalar/vector reads when `VisMF` is constructed.
- Error messages point at missing files, but the file path was corrupted by the plugin.

## Reproduction
1. Use a plotfile header where `m_mf_name[level]` is an absolute Windows path.
2. Open the dataset in VisIt on Windows.
3. Request any field variable.
4. Reader throws `InvalidFilesException` due to prefixed/invalid path.

## Proposed Fix
Use the existing cross-platform `IsAbsolutePath(...)` and `JoinPath(...)` helpers in both call sites.

```diff
diff --git a/avtamrexFileFormat.C b/avtamrexFileFormat.C
index e79ee2a..f3f7079 100644
--- a/avtamrexFileFormat.C
+++ b/avtamrexFileFormat.C
@@ -1480,8 +1480,8 @@ avtamrexFileFormat::GetVisMF(int timeState, int level) const {
   auto &vismfPtr = plotfileImpl->m_vismf[level];
   if (!vismfPtr) {
     std::string mfName = plotfileImpl->m_mf_name[level];
-    if (!mfName.empty() && mfName[0] != '/') {
-      mfName = plotfilePaths_[timeState] + "/" + mfName;
+    if (!mfName.empty() && !IsAbsolutePath(mfName)) {
+      mfName = JoinPath(plotfilePaths_[timeState], mfName);
     }
     debug1 << "[amrex-plugin] GetVisMF constructing VisMF timeState="
            << timeState << " level=" << level << " mfName='" << mfName
@@ -1857,10 +1857,10 @@ std::string avtamrexFileFormat::GetMultiFabName(int timeState,
   }
 
   std::string mfName = plotfile->m_mf_name[level];
-  if (!mfName.empty() && mfName[0] != '/') {
+  if (!mfName.empty() && !IsAbsolutePath(mfName)) {
     // Plotfile headers store relative MultiFab paths; make them absolute
     // so parallel engines don't depend on the current working directory.
-    mfName = plotfilePaths_[timeState] + "/" + mfName;
+    mfName = JoinPath(plotfilePaths_[timeState], mfName);
   }
   mfNameCache_.emplace(key, mfName);
   return mfName;
```

## Notes
This is platform-specific and does not affect Linux/macOS paths that already begin with `/`.
