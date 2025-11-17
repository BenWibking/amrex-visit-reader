# Particle Species Support Plan

This document outlines the work required to add VisIt-facing particle-species
support to the AMReX plotfile reader implemented in `avtamrexFileFormat`.

## Background and Goals

- **Current state.** The plugin only registers AMR meshes and cell-centered
  scalar/vector fields exposed by `amrex::PlotFileData`. Particle species in
  AMReX plotfiles are ignored, which is explicitly called out in
  `README.md`.
- **Goal.** Allow VisIt users to open `.amrex` descriptors and request particle
  point-clouds (positions + per-particle attributes) for any species stored in
  the plotfile. This requires adding metadata entries, reading particle data per
  timestep/domain, and returning them through VisIt's particle mesh interface.
- **Datasets.** Two fixtures already cover particle use cases:
  `example_data/Nyx_LyA` (dark-matter particles bundled with grid-derived
  diagnostics) and `example_data/StarParticles` (stellar particle dump with many
  attributes). Plan to rely on both for validation.

## Constraints and Design Considerations

- Stay aligned with VisIt's expectations: each particle species is typically
  exposed as an `AVT_POINT_MESH` with associated point-centered scalars/vectors.
- Continue lazy loading and caching patterns already present for field data.
- Avoid regressing existing AMR mesh behavior. Particles should be additive.
- Support multiple species, multiple attributes per species, and multi-level
  particle sharding.
- Anticipate large particle counts; streaming and caching must avoid
  duplicating full datasets unnecessarily.

## Implementation Phases

### 1. Inspect AMReX particle APIs

1. Review `amrex::PlotFileData` interfaces for particle metadata (e.g.
   `plotfile->particleNames()`, `particleLevels()`, `particleCounts()` if
   available). If the helper lacks what we need, fall back to lower-level
   AMReX readers (`amrex::ParticleContainer`, `ParticleDataAdaptor`).
2. Prototype (possibly in a standalone utility under `example_data/`) to decode
   the `StarParticles` dataset and confirm the API surface can iterate species,
   retrieve positions, velocities, and arbitrary real/integer attributes.

### 2. Data-model extensions inside `avtamrexFileFormat`

1. Extend `DatasetType` with a `Particle` entry to distinguish between AMR
   meshes and particle clouds (`avtamrexFileFormat.h`).
2. Introduce data structures to track per-species metadata:
   ```c++
   struct ParticleSpeciesInfo {
     std::string visitMeshName;
     std::vector<std::string> realComponents;
     std::vector<std::string> intComponents;
     int spatialDim;
   };
   std::unordered_map<std::string, ParticleSpeciesInfo> particleMap_;
   ```
3. Cache particle data similarly to `fieldDataCache_`, keyed by
   `(timeState, speciesName, levelOrShard)` to prevent repeated disk reads.
4. Ensure `FreeUpResources()` drops the new caches to cap memory usage.

### 3. Populate particle metadata

1. During `PopulateHierarchyCache`/`BuildFieldHierarchy`, query the plotfile for
   available particle species. Each species should register:
   - A mesh entry via `AddMeshToMetaData` with type `AVT_POINT_MESH`.
   - A default coordinate variable (probably `xyz` vector) so VisIt knows how
     to place the particles.
2. For every per-particle attribute (reals and integers) register scalar or
   vector variables using `AddScalarVarToMetaData`/`AddVectorVarToMetaData`,
   referencing the new particle mesh name. Adopt naming like
   `<species>/<attribute>` to avoid collisions with field variables.
3. Store the mesh-to-species relationship inside `particleMap_` so
   `GetMesh/GetVar/GetVectorVar` can route requests appropriately.

### 4. Implement particle loading

1. Add a helper that, given `(timeState, speciesName, shardIndex)`, loads the
   particle buffer from AMReX (positions + attributes) into a VisIt-friendly
   struct. Use AMReX iterators to respect level/box decomposition so VisIt's
   domain-based execution model stays consistent.
2. Update `GetMesh` to detect when the requested mesh is particle-backed and
   construct a `vtkUnstructuredGrid` or `vtkPolyData` with vertices only (one
   point per particle). Include a global ID array if available.
3. Update `GetVar`/`GetVectorVar` to recognize particle variables and copy the
   attribute data into `vtkFloatArray`/`vtkDoubleArray`. Ensure tuple counts
   match the particle count in the corresponding domain.
4. Handle ghost/overlap notions: particle datasets usually do not provide ghost
  zones, so mark them accordingly (or omit ghost arrays).

### 5. Testing and validation

1. Expand `example_data/` instructions to mention which `.amrex` descriptors
   exercise particles and what plots to expect.
2. Manually open `nyx_dataset.amrex` and `orion_dataset.amrex` in VisIt, then
   verify:
   - Particle meshes appear in the database tree.
   - Position plots render point clouds in the correct domain extents.
   - Scalar/vector attributes match known values (spot-check against AMReX
     reference scripts or small particle subsets).
3. Consider adding a lightweight command-line check (e.g. Python script that
   invokes the reader via libsim) if feasible.

### 6. Documentation and cleanup

1. Update `README.md` “Supported / Not yet supported” tables to reflect the new
   capability once complete.
2. Document any limitations (e.g. unsupported particle topology, required AMReX
   version).
3. Add inline comments where the particle-path diverges significantly from the
   existing mesh code.

## Open Questions / Follow-ups

1. **AMReX API coverage.** Does the targeted AMReX version expose all needed
   particle-reading helpers through `PlotFileData`, or do we need to vendor
   additional headers/sources?
2. **Domain decomposition.** Should particle domains match the AMR box
   decomposition, or should we create one VisIt domain per species regardless
   of AMR layout? The former aligns with AMReX’s sharding but may require extra
   bookkeeping.
3. **Performance.** Large particle dumps could exceed VisIt’s default memory
   budgets. Consider optional decimation, species filters, or pagination later.
4. **Attribute typing.** AMReX distinguishes real vs. integer particle
   components. Decide whether to expose integers as-is or convert to floats for
   VisIt compatibility.

Tracking and resolving these questions early will keep the implementation
focused and prevent rework once coding begins.
