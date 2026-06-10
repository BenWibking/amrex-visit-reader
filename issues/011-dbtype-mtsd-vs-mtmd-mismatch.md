# Plugin declares `DB_TYPE_MTSD` but implements an MTMD file format

- Severity: Low
- Component: `amrex.xml`, `amrexCommonPluginInfo.C` (`GetDatabaseType`, `SetupDatabase`)

## Summary
`amrex.xml` declares `dbtype="MTSD"`, and the generated `amrexCommonPluginInfo::GetDatabaseType()` accordingly returns `DB_TYPE_MTSD` (multiple timesteps, single domain). But the reader is `avtamrexFileFormat : public avtMTMDFileFormat`, and `SetupDatabase` constructs an `avtMTMDFileFormatInterface` — a multi-domain format (one domain per AMR patch).

The implementation and the declared database type disagree; `SetupDatabase` was evidently hand-adapted to MTMD while the XML was left at MTSD.

## Impact
- Anything in VisIt that consults `GetDatabaseType()` (database grouping, virtual-database time-series heuristics) sees the wrong type. In practice the overridden `SetupDatabase` hides most consequences, which is why this is Low severity.
- Per `AGENTS.md`, the `amrex*PluginInfo` files are supposed to be regenerated from `amrex.xml` when definitions change. Regenerating from the current XML would emit an `avtMTSDFileFormatInterface`-based `SetupDatabase` that no longer matches `avtamrexFileFormat`, breaking the build (or, worse, compiling against the wrong base if the reader were also regenerated).

## Reproduction
1. Run VisIt's `xml2info` on `amrex.xml`.
2. The regenerated `amrexCommonPluginInfo::SetupDatabase` uses the MTSD interface and fails to compile against `avtamrexFileFormat` (an `avtMTMDFileFormat`).

## Proposed Fix
Update the XML to match the implementation and regenerate (or hand-apply the same change to the generated file):

```diff
diff --git a/amrex.xml b/amrex.xml
--- a/amrex.xml
+++ b/amrex.xml
@@ -1,5 +1,5 @@
 <?xml version="1.0"?>
-  <Plugin name="amrex" type="database" label="" version="" enabled="true" mdspecificcode="false" engspecificcode="false" onlyengine="false" noengine="false" dbtype="MTSD" haswriter="false" hasoptions="false" haslicense="false" filePatternsStrict="false" opensWholeDirectory="true">
+  <Plugin name="amrex" type="database" label="" version="" enabled="true" mdspecificcode="false" engspecificcode="false" onlyengine="false" noengine="false" dbtype="MTMD" haswriter="false" hasoptions="false" haslicense="false" filePatternsStrict="false" opensWholeDirectory="true">
```

```diff
diff --git a/amrexCommonPluginInfo.C b/amrexCommonPluginInfo.C
--- a/amrexCommonPluginInfo.C
+++ b/amrexCommonPluginInfo.C
@@ -21,7 +21,7 @@ DatabaseType
 amrexCommonPluginInfo::GetDatabaseType()
 {
-    return DB_TYPE_MTSD;
+    return DB_TYPE_MTMD;
 }
```
