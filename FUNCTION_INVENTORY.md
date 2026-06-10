<!-- ABOUTME: Inventory of all functions in this repository (excluding vendored extern/) for the bug review. -->
<!-- ABOUTME: Each function is checked off after it has been manually read during the review pass. -->

# Function Inventory — Bug Review

Scope: plugin sources, scripts, and helpers. Vendored code under `extern/` is excluded.

## avtamrexFileFormat.C — anonymous-namespace helpers

- [x] `ComputeLogicalExtents` (line 79)
- [x] `IsChildPatch` (line 102)
- [x] `MeshIsNodeCentered` (line 112)
- [x] `ComputeGlobalCellDimensions` (line 122)
- [x] `ComputeGlobalNodeDimensions` (line 147)
- [x] `ComputePatchCellCounts` (line 162)
- [x] `ComputePatchNodeCounts` (line 182)
- [x] `IsSeparator` (line 205)
- [x] `ParsePattern` (line 209)
- [x] `AmrexRuntime::Retain` / `AmrexRuntime::Release` (lines 249, 264)
- [x] `MakeRefinementVector` (line 286)
- [x] `MakeCellSizeVector` (line 296)
- [x] `MakeRefRatioIntVect` (line 306)
- [x] `WriteStderr` (line 310)
- [x] `SegvHandlerActiveFlag` (line 326)
- [x] `PrintSegvStackTrace` (line 331)
- [x] `SegvSignalHandler` (line 348)
- [x] `InstallSegfaultTraceHandler` (line 363)
- [x] `DeleteStructuredDomainNesting` (line 385)
- [x] `JoinContainer` (line 390)
- [x] `JoinArray` (line 404)
- [x] `CenteringToString` (line 417)
- [x] `LogPatchSummary` (line 434)
- [x] `JoinStrings` (line 447)
- [x] `ParticleDataFileName` (line 468)
- [x] `ResolveParticleDataFileName` (line 476)
- [x] `ParseParticleHeader` (line 522)
- [x] `ParticleRealIndex` (line 728)
- [x] `ParticleIntIndex` (line 733)
- [x] `IsVersionTwoDotOne` (line 738)
- [x] `UnpackParticleIdFromIdCpu` (line 742)
- [x] `UnpackParticleCpuFromIdCpu` (line 750)
- [x] `ParticleIntValue` (line 756)
- [x] `NextParticleOffsetForFile` (line 781)
- [x] `ReadParticleBlock` (line 809)
- [x] `IsAbsolutePath` (line 943)
- [x] `ParentDirectory` (line 962)
- [x] `EndsWithSeparator` (line 983)
- [x] `TrimTrailingSeparators` (line 991)
- [x] `StripLeadingSeparators` (line 1008)
- [x] `Basename` (line 1016)
- [x] `Stem` (line 1034)
- [x] `PathExists` (line 1046)
- [x] `PathIsDirectory` (line 1056)
- [x] `JoinPath` (line 1072)
- [x] `ParsePlotfileDirectoryName` (line 1097)
- [x] `ListDirectoryEntries` (line 1122)

## avtamrexFileFormat.C — class methods

- [x] `ResolveDescriptorPaths` (line 1172) — unreferenced, see issue 010
- [x] `MakeDefaultMeshName` (line 1285)
- [x] `avtamrexFileFormat` constructor (line 1309)
- [x] `~avtamrexFileFormat` destructor (line 1425)
- [x] `GetPlotFile` (line 1432)
- [x] `GetPlotFileImpl` (line 1447)
- [x] `GetVisMF` (line 1462)
- [x] `GetParticleMesh` (line 1499)
- [x] `GetParticleVar` (line 1582)
- [x] `GetParticleVectorVar` (line 1687)
- [x] `QueueVisMFClear` (line 1822)
- [x] `GetMultiFabName` (line 1827)
- [x] `GetNTimesteps` (line 1880)
- [x] `FreeUpResources` (line 1898)
- [x] `PopulateHierarchyCache` (line 1942)
- [x] `BuildFieldHierarchy` (line 1982) — see issue 007
- [x] `BuildParticleHierarchy` (line 2030)
- [x] `RegisterFieldVariables` (line 2264)
- [x] `BuildHierarchyFromPlotfile` (line 2314)
- [x] `PopulateDatabaseMetaData` (line 2394) — see issue 007
- [x] `GetCycles` (line 2428) — see issue 006
- [x] `GetTimes` (line 2435)
- [x] `EnsureHierarchyInitialized` (line 2446)
- [x] `EnsureParticleHierarchyInitialized` (line 2465)
- [x] `EnsureParticleVarMapsInitialized` (line 2488)
- [x] `GetAuxiliaryData` (line 2518)
- [x] `ParseMeshLevel` (line 2698) — unreferenced, see issue 010
- [x] `CreateRectilinearPatch` (line 2723)
- [x] `BuildDomainNesting` (line 2775)
- [x] `BuildDomainBoundaryList` (line 2856) — unreferenced, see issue 010
- [x] `BuildStructuredDomainBoundaries` (line 2983)
- [x] `BuildGlobalZoneIds` (line 3025) — see issue 009
- [x] `BuildGlobalNodeIds` (line 3077) — see issue 009
- [x] `AddGhostZonesForPatch` (line 3128)
- [x] `LoadScalarPatchData` (line 3239) — see issue 008
- [x] `LoadVectorPatchData` (line 3339)
- [x] `GetMesh` (line 3473) — see issues 002, 007
- [x] `GetVar` (line 3575)
- [x] `GetVectorVar` (line 3668)

## avtamrexFileFormat.h

- [x] inline members / struct definitions (whole header)

## amrexCommonPluginInfo.C

- [x] `GetDatabaseType` (line 21) — see issue 011
- [x] `SetupDatabase` (line 44)

## amrexEnginePluginInfo.C

- [x] `GetWriter` (line 22)

## amrexMDServerPluginInfo.C

- [x] `dummy` (line 10)

## amrexPluginInfo.C

- [x] `GetName` (line 30)
- [x] `GetVersion` (line 49)
- [x] `GetID` (line 68)
- [x] `EnabledByDefault` (line 86)
- [x] `HasWriter` (line 104)
- [x] `GetDefaultFilePatterns` (line 119)
- [x] `AreDefaultFilePatternsStrict` (line 140)
- [x] `OpensWholeDirectory` (line 157)

## visitlog.py

- [x] module-level script (no functions) — recorded VisIt session log, not an executable test

## example_data/check_inverse_ghost_zones.py

- [x] `resolve_dataset_path` (line 14)
- [x] `add_scalar_plot` (line 27)
- [x] `query_value` (line 36)
- [x] `main` (line 53) — see issue 003

## example_data/check_parallel_plotfile_open.py

- [x] `resolve_dataset_path` (line 14)
- [x] `ensure_parallel_engine` (line 27)
- [x] `pick_scalar_variable` (line 57)
- [x] `main` (line 70)

## example_data/check_particle_plotfile_open.py

- [x] `resolve_dataset_path` (line 14)
- [x] `ensure_parallel_engine` (line 27)
- [x] `iter_metadata_items` (line 57)
- [x] `get_mesh_type` (line 64)
- [x] `get_mesh_name` (line 72)
- [x] `find_point_meshes` (line 79)
- [x] `vars_for_mesh` (line 89)
- [x] `render_plot` (line 97)
- [x] `query_minmax` (line 123)
- [x] `get_vector_component_count` (line 144)
- [x] `sanitize_expr_name` (line 156)
- [x] `define_vector_component_expr` (line 160)
- [x] `main` (line 170)

## example_data/check_particle_plotfile_clearcache.py

- [x] `resolve_dataset_path` (line 14)
- [x] `ensure_parallel_engine` (line 27)
- [x] `iter_metadata_items` (line 57)
- [x] `get_mesh_type` (line 64)
- [x] `get_mesh_name` (line 72)
- [x] `find_point_meshes` (line 79)
- [x] `vars_for_mesh` (line 89)
- [x] `pick_first_var` (line 97)
- [x] `query_minmax` (line 102)
- [x] `query_num_nodes` (line 119)
- [x] `define_vector_component_expr` (line 136)
- [x] `main` (line 152) — see issue 004

## example_data/query_numzones.py

- [x] module-level script (no functions)

## Shell scripts

- [x] `build_macos.sh` — issue 005's fix already applied here (`"$@"`)
- [x] `build_andes.sh` — still passes only `$1`, see issue 005
- [x] `fetch_mpich_headers.sh`
- [x] `smoke_test.sh`
- [x] `smoke_test_particles.sh`
- [x] `smoke_test_particles_clearcache.sh`

## Review notes

Issues filed during this pass (see `issues/`):

- 007 — `PopulateDatabaseMetaData` clears `meshMap_` but only repopulates the field mesh on a cold hierarchy cache. Fixed as defensive hardening; verification showed the engine only populates metadata on first visit per timestep, so severity was downgraded to Low (see the issue file).
- 008 — `LoadScalarPatchData` strided copy ignores the offset between the FAB box and the patch box (misaligned data for FABs with ghost cells).
- 009 — `BuildGlobalZoneIds`/`BuildGlobalNodeIds` produce IDs that collide across AMR levels.
- 010 — Unreferenced helpers (`ResolveDescriptorPaths`/`ParsePattern`, `ParseMeshLevel`, `BuildDomainBoundaryList`) contain latent bugs.
- 011 — `amrex.xml` declares `dbtype="MTSD"` while the plugin implements MTMD.
- 012 — `ParsePlotfileDirectoryName` parses unsigned timestep suffixes with `std::stoll`.

Pre-existing issues 001–006 cover: Windows absolute MultiFab paths, `GetMesh` particle lazy-init ordering, inverse-ghost smoke-test exit code, clearcache smoke-test warning downgrade, build scripts dropping extra CMake args, and `GetCycles` int overflow.

Notable non-issues observed (documented, not fixed): the duplicate `AUXILIARY_DATA_DOMAIN_BOUNDARY_INFORMATION` branch in `GetAuxiliaryData` (avtamrexFileFormat.C:2612) is unreachable because the branch at line 2599 already returned; `GetParticleMesh` ignores its `timeState` parameter (harmless — the species info already carries the per-timestep paths).
