// ABOUTME: Declares the read options for the amrex database plugin along with
// ABOUTME: the option-name constants shared with avtamrexFileFormat.

#ifndef AVT_AMREX_OPTIONS_H
#define AVT_AMREX_OPTIONS_H

class DBOptionsAttributes;

// Builds and caches the structured domain boundary object VisIt uses for
// ghost synthesis. Costs O(#domains x #neighbors) memory on every engine
// rank; disable for plotfiles with very large domain counts.
#define AMREX_OPT_DOMAIN_BOUNDARIES \
  "Build domain boundaries for ghost synthesis"

// Declares the mesh structure (patch layout and variable list) identical for
// every timestep, letting VisIt skip re-reading metadata and rebuilding the
// SIL on timestep changes. Only enable when the run never regrids.
#define AMREX_OPT_INVARIANT_MESH \
  "Mesh structure is invariant across timesteps"

DBOptionsAttributes *GetamrexReadOptions(void);

#endif
