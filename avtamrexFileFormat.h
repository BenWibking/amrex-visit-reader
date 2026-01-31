// Copyright (c) Lawrence Livermore National Security, LLC and other VisIt
// Project developers.  See the top-level LICENSE file for dates and other
// details.  No copyright assignment is required to contribute to VisIt.

// ****************************************************************************
//  avtamrexFileFormat.h
// ****************************************************************************

#ifndef AVT_AMREX_FILE_FORMAT_H
#define AVT_AMREX_FILE_FORMAT_H

#include <array>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <AMReX_Box.H>
#include <AMReX_PlotFileUtil.H>

#include <DebugStream.h>
#include <avtLocalStructuredDomainBoundaries.h>
#include <avtMTMDFileFormat.h>
#include <avtStructuredDomainBoundaries.h>
#include <avtStructuredDomainNesting.h>
#include <avtTypes.h>

class vtkDataArray;
class vtkDataSet;
class vtkRectilinearGrid;
class vtkIdTypeArray;

namespace amrex {
class PlotFileDataImpl;
class VisMF;
}

struct PatchInfo {
  int level{0};
  std::vector<long long> offset;
  std::vector<uint64_t> extent;
  amrex::Box cellBox;
  std::vector<long long> storageOffset;
  std::vector<uint64_t> storageExtent;
  std::vector<uint64_t> storageOffsetCanonical;
  std::vector<uint64_t> storageExtentCanonical;
  std::vector<std::string> storageAxisLabels;
  std::array<int, 3> storageToVtk{{0, 1, 2}};
  std::string meshName;
  double origin[3]{0.0, 0.0, 0.0};
  double spacing[3]{0.0, 0.0, 0.0};
  avtCentering centering{AVT_UNKNOWN_CENT};
  int logicalLower[3]{0, 0, 0};
  int logicalUpper[3]{0, 0, 0};
  int fabIndex{-1};
  int spatialDim{0};
};

struct MeshPatchHierarchy {
  std::vector<PatchInfo> patches;
  int numLevels{0};
  std::vector<std::array<int, 3>> levelRefinementRatios;
  std::vector<int> levelIdsPerPatch;
  std::vector<int> groupIds;
  std::vector<std::string> blockNames;
  std::vector<int> levelValues;
  std::vector<std::vector<int>> patchesPerLevel;
  std::vector<std::array<double, 3>> levelCellSizes;
  bool metadataInitialized{false};
  int topologicalDim{0};
  int spatialDim{0};
};

class avtamrexFileFormat : public avtMTMDFileFormat {
public:
  avtamrexFileFormat(const char *);
  ~avtamrexFileFormat() override;

  int GetNTimesteps(void) override;

  const char *GetType(void) override { return "amrex"; }
  void FreeUpResources(void) override;

  vtkDataSet *GetMesh(int, int, const char *) override;
  vtkDataArray *GetVar(int, int, const char *) override;
  vtkDataArray *GetVectorVar(int, int, const char *) override;

  bool HasInvariantMetaData(void) const override { return false; }
  bool HasInvariantSIL(void) const override { return false; }

  void GetCycles(std::vector<int> &) override;
  void GetTimes(std::vector<double> &) override;

  enum class DatasetType { Field = 0, Particle = 1 };

  struct ParticleHeaderInfo {
    int spatialDim{0};
    int numRealExtra{0};
    int numIntExtra{0};
    int numReal{0};
    int numInt{0};
    int finestLevel{0};
    bool isCheckpoint{false};
    bool isSingle{false};
    bool legacy{false};
    long long numParticles{0};
    long long nextId{0};
    std::string version;
    std::vector<std::string> realNames;
    std::vector<std::string> intNames;
    std::vector<int> numGrids;
    std::vector<std::vector<int>> fileNums;
    std::vector<std::vector<int>> particleCounts;
    std::vector<std::vector<long long>> offsets;
  };

  struct ParticleSpeciesInfo {
    std::string meshName;
    std::string speciesName;
    std::string baseMeshName;
    std::vector<std::string> realComponents;
    std::vector<std::string> intComponents;
    ParticleHeaderInfo header;
    std::string speciesDir;
    int spatialDim{0};
    bool legacyHeader{false};
  };

protected:
  // NOTE: VisIt calls this reader from a single thread per instance.
  // The internal caches are not thread-safe by design.
  struct FieldCacheKey {
    int timeState{0};
    int level{0};
    std::string variable;

    bool operator<(const FieldCacheKey &other) const {
      if (timeState != other.timeState) {
        return timeState < other.timeState;
      }
      if (level != other.level) {
        return level < other.level;
      }
      return variable < other.variable;
    }
  };

  struct VisMFCacheKey {
    int timeState{0};
    int level{0};

    bool operator<(const VisMFCacheKey &other) const {
      if (timeState != other.timeState) {
        return timeState < other.timeState;
      }
      return level < other.level;
    }
  };

  struct VisMFClearEntry {
    VisMFCacheKey key;
    int fabIndex{0};
    int compIndex{0};

    bool operator<(const VisMFClearEntry &other) const {
      if (key < other.key) {
        return true;
      }
      if (other.key < key) {
        return false;
      }
      if (fabIndex != other.fabIndex) {
        return fabIndex < other.fabIndex;
      }
      return compIndex < other.compIndex;
    }
  };

  struct ParticleVarInfo {
    std::string meshName;
    std::string speciesName;
    int componentIndex{0};
    bool isReal{true};
  };

  struct ParticleVectorVarInfo {
    std::string meshName;
    std::string speciesName;
    std::vector<int> componentIndices;
    bool isReal{true};
    bool isPosition{false};
  };


  std::vector<std::string> plotfilePaths_;
  std::vector<unsigned long long> iterationIndex_;
  mutable std::vector<double> timeValues_;
  mutable std::vector<std::shared_ptr<amrex::PlotFileData>> plotfileCache_;
  mutable std::vector<std::shared_ptr<amrex::PlotFileDataImpl>>
      plotfileImplCache_;
  mutable std::map<std::pair<int, int>, std::string> mfNameCache_;
  mutable std::map<VisMFCacheKey, std::shared_ptr<amrex::VisMF>> vismfCache_;
  mutable std::vector<VisMFClearEntry> vismfClearList_;
  std::unordered_map<std::string, std::tuple<std::string, std::string>> varMap_;
  std::unordered_map<std::string,
                     std::tuple<std::string, std::vector<std::string>>>
      vectorVarMap_;
  std::unordered_map<std::string, std::tuple<DatasetType, std::string>> meshMap_;
  std::vector<std::unordered_map<std::string, MeshPatchHierarchy>>
      meshHierarchyCache_;
  std::vector<std::unordered_map<std::string, ParticleSpeciesInfo>>
      particleSpeciesCache_;
  std::vector<bool> particleHierarchyInitialized_;
  std::unordered_map<std::string, ParticleVarInfo> particleVarMap_;
  std::unordered_map<std::string, ParticleVectorVarInfo> particleVectorVarMap_;

  void PopulateDatabaseMetaData(avtDatabaseMetaData *, int) override;
  void *GetAuxiliaryData(const char *var, int timestep, int domain,
                         const char *type, void *args,
                         DestructorFunction &) override;

  void EnsureHierarchyInitialized(int timeState);
  void EnsureParticleHierarchyInitialized(int timeState);
  void EnsureParticleVarMapsInitialized(int timeState);
  void PopulateHierarchyCache(int timeState);
  void BuildFieldHierarchy(avtDatabaseMetaData *md,
                           amrex::PlotFileData &plotfile, int timeState);
  void BuildParticleHierarchy(avtDatabaseMetaData *md,
                              amrex::PlotFileData &plotfile, int timeState);
  vtkDataSet *GetParticleMesh(int timeState, int domain,
                              const ParticleSpeciesInfo &species,
                              const MeshPatchHierarchy &hierarchy) const;
  vtkDataArray *GetParticleVar(int timeState, int domain,
                               const ParticleVarInfo &varInfo,
                               const MeshPatchHierarchy &hierarchy) const;
  vtkDataArray *GetParticleVectorVar(int timeState, int domain,
                                     const ParticleVectorVarInfo &varInfo,
                                     const MeshPatchHierarchy &hierarchy) const;
  MeshPatchHierarchy BuildHierarchyFromPlotfile(const std::string &visitMeshName,
                                                amrex::PlotFileData &plotfile);
  std::pair<std::string, int>
  ParseMeshLevel(std::string const &meshName) const;
  vtkDataSet *CreateRectilinearPatch(const PatchInfo &patch) const;
  vtkDataArray *LoadScalarPatchData(int timeState, const PatchInfo &patch,
                                    const std::string &component) const;
  vtkDataArray *LoadVectorPatchData(int timeState, const PatchInfo &patch,
                                    const std::vector<std::string> &components)
      const;
  void RegisterFieldVariables(avtDatabaseMetaData *md,
                              const std::string &meshName,
                              amrex::PlotFileData &plotfile);
  std::shared_ptr<amrex::PlotFileData> GetPlotFile(int timeState) const;
  std::shared_ptr<amrex::PlotFileDataImpl> GetPlotFileImpl(int timeState) const;
  std::shared_ptr<amrex::VisMF> GetVisMF(int timeState, int level) const;
  void QueueVisMFClear(const VisMFCacheKey &key, int fabIndex,
                       int compIndex) const;
  std::string GetMultiFabName(int timeState, int level) const;
  std::vector<std::pair<unsigned long long, std::string>>
  ResolveDescriptorPaths(const std::string &descriptorPath,
                         const std::string &rawPath);
  std::string MakeDefaultMeshName(const std::string &path) const;

  avtStructuredDomainNesting *
  BuildDomainNesting(const MeshPatchHierarchy &hierarchy) const;
  avtLocalStructuredDomainBoundaryList *
  BuildDomainBoundaryList(const MeshPatchHierarchy &hierarchy, int domain) const;
  avtStructuredDomainBoundaries *
  BuildStructuredDomainBoundaries(const MeshPatchHierarchy &hierarchy) const;
  vtkIdTypeArray *BuildGlobalNodeIds(const MeshPatchHierarchy &hierarchy,
                                     int domain) const;
  vtkIdTypeArray *BuildGlobalZoneIds(const MeshPatchHierarchy &hierarchy,
                                     int domain) const;
  void AddGhostZonesForPatch(const MeshPatchHierarchy &hierarchy, int patchIdx,
                             vtkRectilinearGrid *grid) const;
};

#endif
