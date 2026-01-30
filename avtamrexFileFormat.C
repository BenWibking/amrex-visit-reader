// Copyright (c) Lawrence Livermore National Security, LLC and other VisIt
// Project developers.  See the top-level LICENSE file for dates and other
// details.  No copyright assignment is required to contribute to VisIt.

// ****************************************************************************
//  avtamrexFileFormat.C
// ****************************************************************************

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <cerrno>
#ifndef _WIN32
#include <dirent.h>
#else
#include <windows.h>
#endif
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>

#include <avtDatabaseMetaData.h>
#include <vtkCellData.h>
#include <vtkDoubleArray.h>
#include <vtkFloatArray.h>
#include <vtkCellArray.h>
#include <vtkCellType.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPointData.h>
#include <vtkRectilinearGrid.h>
#include <vtkIntArray.h>
#include <vtkLongLongArray.h>
#include <vtkSmartPointer.h>
#include <vtkUnsignedCharArray.h>
#include <vtkIdTypeArray.h>

#include <AMReX.H>
#include <AMReX_Geometry.H>
#include <AMReX_ParallelDescriptor.H>
#include <AMReX_ParIter.H>
#include <AMReX_FArrayBox.H>
#include <AMReX_PlotFileDataImpl.H>
#include <AMReX_VisMF.H>

#include <cpptrace/cpptrace.hpp>

#include <DebugStream.h>
#include <Expression.h>
#include <InvalidVariableException.h>
#include <InvalidFilesException.h>
#include <avtGhostData.h>
#include <avtVariableCache.h>
#include <void_ref_ptr.h>

#include "avtamrexFileFormat.h"

namespace {

inline void ComputeLogicalExtents(PatchInfo &patch) {
  for (int axis = 0; axis < 3; ++axis) {
    int lower = 0;
    if (axis < static_cast<int>(patch.offset.size())) {
      lower = static_cast<int>(patch.offset[axis]);
    }

    int cells = 1;
    if (axis < static_cast<int>(patch.extent.size())) {
      cells = static_cast<int>(patch.extent[axis]);
    }
    if (patch.centering == AVT_NODECENT && cells > 1) {
      cells -= 1;
    }
    if (cells <= 0) {
      cells = 1;
    }

    patch.logicalLower[axis] = lower;
    patch.logicalUpper[axis] = lower + cells - 1;
  }
}

inline bool IsChildPatch(const PatchInfo &coarse, const PatchInfo &fine,
                         const amrex::IntVect &refRatio) {
  if (fine.level != coarse.level + 1) {
    return false;
  }

  amrex::Box fineOnCoarse = amrex::coarsen(fine.cellBox, refRatio);
  return fineOnCoarse.intersects(coarse.cellBox);
}

inline bool MeshIsNodeCentered(const MeshPatchHierarchy &hierarchy) {
  for (const auto &patch : hierarchy.patches) {
    if (patch.centering != AVT_UNKNOWN_CENT) {
      return patch.centering == AVT_NODECENT;
    }
  }
  return false;
}

inline std::array<int, 3>
ComputeGlobalCellDimensions(const MeshPatchHierarchy &hierarchy,
                            bool meshNodeCentered) {
  std::array<int, 3> dims{1, 1, 1};
  for (const auto &patch : hierarchy.patches) {
    for (int axis = 0; axis < 3; ++axis) {
      if (axis >= hierarchy.topologicalDim) {
        dims[axis] = 1;
        continue;
      }
      int limit = patch.logicalUpper[axis] + 1;
      if (meshNodeCentered && limit > 0) {
        limit -= 1;
      }
      if (limit > dims[axis]) {
        dims[axis] = limit;
      }
    }
  }
  for (int axis = hierarchy.topologicalDim; axis < 3; ++axis) {
    dims[axis] = 1;
  }
  return dims;
}

inline std::array<int, 3>
ComputeGlobalNodeDimensions(const MeshPatchHierarchy &hierarchy,
                            bool meshNodeCentered) {
  std::array<int, 3> cellDims =
      ComputeGlobalCellDimensions(hierarchy, meshNodeCentered);
  std::array<int, 3> nodeDims{1, 1, 1};
  for (int axis = 0; axis < 3; ++axis) {
    if (axis >= hierarchy.topologicalDim) {
      nodeDims[axis] = 1;
    } else {
      nodeDims[axis] = cellDims[axis] + (meshNodeCentered ? 0 : 1);
    }
  }
  return nodeDims;
}

inline std::array<int, 3> ComputePatchCellCounts(const PatchInfo &patch,
                                                 int topoDim,
                                                 bool meshNodeCentered) {
  std::array<int, 3> counts{1, 1, 1};
  for (int axis = 0; axis < 3; ++axis) {
    if (axis >= topoDim) {
      counts[axis] = 1;
      continue;
    }
    counts[axis] = patch.logicalUpper[axis] - patch.logicalLower[axis] + 1;
    if (meshNodeCentered && counts[axis] > 0) {
      counts[axis] -= 1;
    }
    if (counts[axis] <= 0) {
      counts[axis] = 1;
    }
  }
  return counts;
}

inline std::array<int, 3> ComputePatchNodeCounts(const PatchInfo &patch,
                                                 int topoDim,
                                                 bool meshNodeCentered) {
  std::array<int, 3> cellCounts =
      ComputePatchCellCounts(patch, topoDim, meshNodeCentered);
  std::array<int, 3> nodeCounts{1, 1, 1};
  for (int axis = 0; axis < 3; ++axis) {
    if (axis >= topoDim) {
      nodeCounts[axis] = 1;
    } else {
      nodeCounts[axis] = cellCounts[axis] + (meshNodeCentered ? 0 : 1);
    }
  }
  return nodeCounts;
}

struct PathPattern {
  std::string prefix;
  std::string suffix;
  int width{0};
  bool hasPattern{false};
};

inline bool IsSeparator(char c) {
  return c == '/' || c == '\\';
}

inline PathPattern ParsePattern(const std::string &path) {
  PathPattern pattern;
  const auto percent = path.find('%');
  if (percent == std::string::npos) {
    pattern.prefix = path;
    return pattern;
  }

  size_t idx = percent + 1;
  if (idx >= path.size()) {
    pattern.prefix = path;
    return pattern;
  }

  if (path[idx] == '0') {
    ++idx;
  }

  size_t widthStart = idx;
  while (idx < path.size() && std::isdigit(static_cast<unsigned char>(path[idx]))) {
    ++idx;
  }
  if (idx > widthStart) {
    pattern.width = std::stoi(path.substr(widthStart, idx - widthStart));
  }

  if (idx >= path.size() || (path[idx] != 'T' && path[idx] != 't')) {
    pattern.prefix = path;
    return pattern;
  }
  ++idx;

  pattern.hasPattern = true;
  pattern.prefix = path.substr(0, percent);
  pattern.suffix = path.substr(idx);
  return pattern;
}

class AmrexRuntime {
public:
  static void Retain() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (refCount_ == 0) {
      int argc = 0;
      char **argv = nullptr;
#ifdef AMREX_USE_MPI
      amrex::Initialize(argc, argv, false, MPI_COMM_WORLD);
#else
      amrex::Initialize(argc, argv, false);
#endif
      initialized_ = true;
    }
    ++refCount_;
  }

  static void Release() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (refCount_ > 0) {
      --refCount_;
      if (refCount_ == 0 && initialized_) {
        amrex::Finalize();
        initialized_ = false;
      }
    }
  }

private:
  static std::mutex mutex_;
  static int refCount_;
  static bool initialized_;
};

std::mutex AmrexRuntime::mutex_;
int AmrexRuntime::refCount_ = 0;
bool AmrexRuntime::initialized_ = false;

template <typename T>
inline void DuplicateHighEndNodes(const PatchInfo &patch, T *values) {
  if (values == nullptr || patch.centering != AVT_NODECENT) {
    return;
  }

  std::array<uint64_t, 3> vtkDims{1, 1, 1};
  for (int axis = 0; axis < 3; ++axis) {
    if (axis < static_cast<int>(patch.extent.size())) {
      uint64_t dim = patch.extent[axis];
      vtkDims[axis] = dim == 0 ? 1 : dim;
    }
  }

  const auto &storageDims = patch.storageExtentCanonical;
  const auto &storageToVtk = patch.storageToVtk;
  std::array<uint64_t, 3> validDims{1, 1, 1};
  bool requiresDuplication = false;
  for (int axis = 0; axis < 3; ++axis) {
    int storageIndex = storageToVtk[axis];
    uint64_t dimValue = vtkDims[axis];
    if (storageIndex >= 0 &&
        storageIndex < static_cast<int>(storageDims.size())) {
      dimValue = storageDims[static_cast<size_t>(storageIndex)];
    }
    if (dimValue == 0) {
      dimValue = 1;
    }
    validDims[axis] = dimValue;
    if (vtkDims[axis] > dimValue) {
      requiresDuplication = true;
    }
  }

  if (!requiresDuplication) {
    return;
  }

  uint64_t totalElements = vtkDims[0] * vtkDims[1] * vtkDims[2];
  for (uint64_t linear = 0; linear < totalElements; ++linear) {
    uint64_t tmp = linear;
    std::array<uint64_t, 3> coords{0, 0, 0};
    for (int axis = 0; axis < 3; ++axis) {
      const uint64_t dim = vtkDims[axis];
      if (dim > 0) {
        coords[axis] = tmp % dim;
        tmp /= dim;
      } else {
        coords[axis] = 0;
      }
    }

    bool needsCopy = false;
    std::array<uint64_t, 3> srcCoords = coords;
    for (int axis = 0; axis < 3; ++axis) {
      uint64_t valid = validDims[axis];
      if (coords[axis] >= valid) {
        needsCopy = true;
        srcCoords[axis] = valid > 0 ? valid - 1 : 0;
      }
    }

    if (!needsCopy) {
      continue;
    }

    uint64_t srcIndex =
        srcCoords[0] + vtkDims[0] * (srcCoords[1] + vtkDims[1] * srcCoords[2]);
    values[linear] = values[static_cast<size_t>(srcIndex)];
  }
}

inline std::vector<int> MakeRefinementVector(const std::array<int, 3> &ratio,
                                             int dims) {
  std::vector<int> result(std::max(1, dims), 1);
  for (int axis = 0; axis < std::min(3, static_cast<int>(result.size()));
       ++axis) {
    result[axis] = ratio[axis];
  }
  return result;
}

inline std::vector<double> MakeCellSizeVector(const std::array<double, 3> &sizes,
                                              int dims) {
  std::vector<double> result(std::max(1, dims), 0.0);
  for (int axis = 0; axis < std::min(3, static_cast<int>(result.size()));
       ++axis) {
    result[axis] = sizes[axis];
  }
  return result;
}

inline amrex::IntVect MakeRefRatioIntVect(const std::array<int, 3> &ratio) {
  return amrex::IntVect(AMREX_D_DECL(ratio[0], ratio[1], ratio[2]));
}

inline void WriteStderr(const char *message, size_t length) {
  if (message == nullptr || length == 0) {
    return;
  }
  ssize_t remaining = static_cast<ssize_t>(length);
  const char *ptr = message;
  while (remaining > 0) {
    ssize_t written = ::write(STDERR_FILENO, ptr, static_cast<size_t>(remaining));
    if (written <= 0) {
      break;
    }
    remaining -= written;
    ptr += written;
  }
}

std::atomic<bool> &SegvHandlerActiveFlag() {
  static std::atomic<bool> flag{false};
  return flag;
}

void PrintSegvStackTrace() {
  constexpr char header[] =
      "\n[amrex-plugin] cpptrace captured stack trace (signal handler):\n";
  WriteStderr(header, sizeof(header) - 1);
  try {
    auto trace = cpptrace::generate_trace();
    std::string traceString = trace.to_string();
    WriteStderr(traceString.c_str(), traceString.size());
    const char newline = '\n';
    WriteStderr(&newline, 1);
  } catch (...) {
    constexpr char failure[] =
        "[amrex-plugin] cpptrace failed to generate stack trace after signal.\n";
    WriteStderr(failure, sizeof(failure) - 1);
  }
}

void SegvSignalHandler(int sig, siginfo_t *, void *) {
  auto &active = SegvHandlerActiveFlag();
  bool expected = false;
  if (active.compare_exchange_strong(expected, true)) {
    PrintSegvStackTrace();
  }

  struct sigaction defaultAction;
  std::memset(&defaultAction, 0, sizeof(defaultAction));
  defaultAction.sa_handler = SIG_DFL;
  sigemptyset(&defaultAction.sa_mask);
  sigaction(sig, &defaultAction, nullptr);
  raise(sig);
}

void InstallSegfaultTraceHandler() {
  static std::atomic<bool> installed{false};
  bool expected = false;
  if (!installed.compare_exchange_strong(expected, true)) {
    return;
  }

  cpptrace::register_terminate_handler();
  (void)cpptrace::generate_trace();

  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_sigaction = &SegvSignalHandler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO;
  if (sigaction(SIGSEGV, &action, nullptr) != 0) {
    constexpr char failure[] =
        "[amrex-plugin] Failed to register cpptrace SIGSEGV handler.\n";
    WriteStderr(failure, sizeof(failure) - 1);
  }
}

void DeleteStructuredDomainNesting(void *ptr) {
  delete static_cast<avtStructuredDomainNesting *>(ptr);
}

template <typename Container>
inline std::string JoinContainer(const Container &values) {
  std::ostringstream oss;
  oss << '[';
  for (size_t idx = 0; idx < values.size(); ++idx) {
    if (idx > 0) {
      oss << ',';
    }
    oss << values[idx];
  }
  oss << ']';
  return oss.str();
}

template <typename T, size_t N>
inline std::string JoinArray(const T (&values)[N]) {
  std::ostringstream oss;
  oss << '[';
  for (size_t idx = 0; idx < N; ++idx) {
    if (idx > 0) {
      oss << ',';
    }
    oss << values[idx];
  }
  oss << ']';
  return oss.str();
}

inline const char *CenteringToString(avtCentering cent) {
  switch (cent) {
  case AVT_NODECENT:
    return "node";
  case AVT_ZONECENT:
    return "zone";
  case AVT_UNKNOWN_CENT:
    return "unknown";
#ifdef AVT_NO_CENTERING
  case AVT_NO_CENTERING:
    return "none";
#endif
  default:
    return "other";
  }
}

inline void LogPatchSummary(const PatchInfo &patch,
                            const std::string &context) {
  debug3 << "[amrex-plugin] " << context
         << " mesh=" << patch.meshName << " level=" << patch.level
         << " centering=" << CenteringToString(patch.centering)
         << " offset=" << JoinContainer(patch.offset)
         << " extent=" << JoinContainer(patch.extent)
         << " origin=" << JoinArray(patch.origin)
         << " spacing=" << JoinArray(patch.spacing)
         << " logicalLower=" << JoinArray(patch.logicalLower)
         << " logicalUpper=" << JoinArray(patch.logicalUpper) << '\n';
}

inline std::string JoinStrings(const std::vector<std::string> &values) {
  std::ostringstream oss;
  oss << '[';
  for (size_t idx = 0; idx < values.size(); ++idx) {
    if (idx > 0) {
      oss << ", ";
    }
    oss << values[idx];
  }
  oss << ']';
  return oss.str();
}

inline bool ParseParticleHeader(const std::string &headerPath,
                                avtamrexFileFormat::ParticleHeaderInfo &info,
                                std::string &error) {
  error.clear();
  info = avtamrexFileFormat::ParticleHeaderInfo{};

  std::ifstream header(headerPath);
  if (!header.is_open()) {
    error = "Failed to open particle header '" + headerPath + "'.";
    return false;
  }

  std::vector<std::string> tokens;
  std::string token;
  while (header >> token) {
    tokens.push_back(token);
  }
  if (tokens.size() < 3) {
    std::ostringstream oss;
    oss << "Particle header '" << headerPath
        << "' is too short (" << tokens.size() << " tokens).";
    error = oss.str();
    return false;
  }

  const std::string &version = tokens[0];
  info.version = version;
  const bool isLegacyVersion =
      (version.find("Version_One_Dot_Zero") != std::string::npos) ||
      (version.find("Version_One_Dot_One") != std::string::npos);
  const bool isModernVersion =
      (version.find("Version_Two_Dot_Zero") != std::string::npos) ||
      (version.find("Version_Two_Dot_One") != std::string::npos);
  const bool isUnknownVersion = !isLegacyVersion && !isModernVersion;
  info.isSingle = (version.find("single") != std::string::npos);

  auto parseInt = [&](size_t idx, int &value) -> bool {
    if (idx >= tokens.size()) {
      return false;
    }
    try {
      value = std::stoi(tokens[idx]);
    } catch (const std::exception &) {
      return false;
    }
    return true;
  };
  auto parseLong = [&](size_t idx, long long &value) -> bool {
    if (idx >= tokens.size()) {
      return false;
    }
    try {
      value = std::stoll(tokens[idx]);
    } catch (const std::exception &) {
      return false;
    }
    return true;
  };

  int dm = 0;
  int nr = 0;
  if (!parseInt(1, dm) || !parseInt(2, nr)) {
    error = "Failed to parse particle header prefix from '" + headerPath + "'.";
    return false;
  }

  auto tryModernFormat = [&]() -> bool {
    size_t idx = 3;
    if (nr < 0 || tokens.size() < idx + static_cast<size_t>(nr) + 1) {
      return false;
    }
    const int numRealExtra = nr;
    std::vector<std::string> realNames;
    realNames.reserve(static_cast<size_t>(nr));
    for (int i = 0; i < nr; ++i) {
      realNames.push_back(tokens[idx++]);
    }
    int ni = 0;
    if (!parseInt(idx, ni)) {
      return false;
    }
    ++idx;
    if (ni < 0 || tokens.size() < idx + static_cast<size_t>(ni) + 3) {
      return false;
    }
    const int numIntExtra = ni;
    std::vector<std::string> intNames;
    intNames.reserve(static_cast<size_t>(ni));
    for (int i = 0; i < ni; ++i) {
      intNames.push_back(tokens[idx++]);
    }
    int checkpointFlag = 0;
    long long numParticles = 0;
    long long maxNextId = 0;
    int finestLevel = 0;
    if (!parseInt(idx, checkpointFlag)) {
      return false;
    }
    ++idx;
    if (!parseLong(idx, numParticles)) {
      return false;
    }
    ++idx;
    if (!parseLong(idx, maxNextId)) {
      return false;
    }
    ++idx;
    if (!parseInt(idx, finestLevel)) {
      return false;
    }
    ++idx;
    if (finestLevel < 0) {
      return false;
    }
    if (tokens.size() < idx + static_cast<size_t>(finestLevel + 1)) {
      return false;
    }

    std::vector<int> numGrids;
    numGrids.reserve(static_cast<size_t>(finestLevel + 1));
    long long totalGrids = 0;
    for (int lev = 0; lev <= finestLevel; ++lev) {
      int ngrids = 0;
      if (!parseInt(idx, ngrids)) {
        return false;
      }
      ++idx;
      if (ngrids < 0) {
        return false;
      }
      numGrids.push_back(ngrids);
      totalGrids += ngrids;
    }

    std::vector<std::vector<int>> fileNums;
    std::vector<std::vector<int>> particleCounts;
    std::vector<std::vector<long long>> offsets;
    fileNums.resize(static_cast<size_t>(finestLevel + 1));
    particleCounts.resize(static_cast<size_t>(finestLevel + 1));
    offsets.resize(static_cast<size_t>(finestLevel + 1));

    for (int lev = 0; lev <= finestLevel; ++lev) {
      fileNums[lev].reserve(static_cast<size_t>(numGrids[lev]));
      particleCounts[lev].reserve(static_cast<size_t>(numGrids[lev]));
      offsets[lev].reserve(static_cast<size_t>(numGrids[lev]));
      for (int grid = 0; grid < numGrids[lev]; ++grid) {
        int fileNum = 0;
        int count = 0;
        long long offset = 0;
        if (!parseInt(idx, fileNum)) {
          return false;
        }
        ++idx;
        if (!parseInt(idx, count)) {
          return false;
        }
        ++idx;
        if (!parseLong(idx, offset)) {
          return false;
        }
        ++idx;
        fileNums[lev].push_back(fileNum);
        particleCounts[lev].push_back(count);
        offsets[lev].push_back(offset);
      }
    }

    info.spatialDim = dm;
    info.numRealExtra = numRealExtra;
    info.numIntExtra = numIntExtra;
    info.numReal = dm + numRealExtra;
    info.numInt = 2 * (checkpointFlag != 0) + numIntExtra;
    info.isCheckpoint = (checkpointFlag != 0);
    info.finestLevel = finestLevel;
    info.numParticles = numParticles;
    info.nextId = maxNextId;
    info.realNames = std::move(realNames);
    info.intNames = std::move(intNames);
    info.numGrids = std::move(numGrids);
    info.fileNums = std::move(fileNums);
    info.particleCounts = std::move(particleCounts);
    info.offsets = std::move(offsets);
    return true;
  };

  if (isModernVersion && tryModernFormat()) {
    info.legacy = false;
    return true;
  }

  auto tryLegacyFormat = [&]() -> bool {
    size_t idx = 3;
    long long numParticles = 0;
    long long maxNextId = 0;
    int finestLevel = 0;
    if (!parseLong(idx, numParticles)) {
      return false;
    }
    ++idx;
    if (!parseLong(idx, maxNextId)) {
      return false;
    }
    ++idx;
    if (!parseInt(idx, finestLevel)) {
      return false;
    }
    ++idx;

    if (finestLevel < 0) {
      return false;
    }

    if (tokens.size() < idx + static_cast<size_t>(finestLevel + 1)) {
      return false;
    }
    std::vector<int> numGrids;
    numGrids.reserve(static_cast<size_t>(finestLevel + 1));
    long long totalGrids = 0;
    for (int lev = 0; lev <= finestLevel; ++lev) {
      int ngrids = 0;
      if (!parseInt(idx, ngrids)) {
        return false;
      }
      ++idx;
      if (ngrids < 0) {
        return false;
      }
      numGrids.push_back(ngrids);
      totalGrids += ngrids;
    }

    const size_t remaining = tokens.size() - idx;
    if (remaining < static_cast<size_t>(totalGrids) * 3) {
      return false;
    }

    info.spatialDim = dm;
    info.numRealExtra = nr;
    info.numIntExtra = 0;
    info.numReal = dm + nr;
    info.numInt = 0;
    info.finestLevel = finestLevel;
    info.legacy = true;
    info.isCheckpoint = false;
    info.numParticles = numParticles;
    info.nextId = maxNextId;
    info.realNames.clear();
    info.realNames.reserve(static_cast<size_t>(std::max(0, nr)));
    for (int i = 0; i < nr; ++i) {
      std::ostringstream name;
      name << "real" << i;
      info.realNames.push_back(name.str());
    }
    info.intNames.clear();
    info.numGrids = std::move(numGrids);
    return true;
  };

  if ((isLegacyVersion || isUnknownVersion) && tryLegacyFormat()) {
    return true;
  }

  error = "Unrecognized particle header format in '" + headerPath + "'.";
  return false;
}

struct LegacyParticleHeader {
  std::string version;
  int spatialDim{0};
  int numReal{0};
  long long numParticles{0};
  long long maxNextId{0};
  int finestLevel{0};
  std::vector<int> gridsPerLevel;
  struct GridEntry {
    int which{0};
    int count{0};
    long long where{0};
  };
  std::vector<GridEntry> gridEntries;
};

struct LegacyParticleBlock {
  int count{0};
  int extraComps{0};
  int spatialDim{0};
  std::vector<double> positions;
  std::vector<double> extras;
};

struct ParticleBlock {
  int count{0};
  int numReal{0};
  int numInt{0};
  int spatialDim{0};
  bool isSingle{false};
  int intBytes{0};
  std::vector<long long> intData;
  std::vector<float> realDataSingle;
  std::vector<double> realDataDouble;
};

inline long long NextParticleOffsetForFile(
    const avtamrexFileFormat::ParticleHeaderInfo &header, int level,
    int fileNum, long long offset) {
  long long nextOffset = -1;
  if (level < 0 ||
      static_cast<size_t>(level) >= header.fileNums.size() ||
      static_cast<size_t>(level) >= header.offsets.size()) {
    return nextOffset;
  }
  const auto &fileNums = header.fileNums[static_cast<size_t>(level)];
  const auto &offsets = header.offsets[static_cast<size_t>(level)];
  for (size_t grid = 0; grid < fileNums.size(); ++grid) {
    if (fileNums[grid] != fileNum) {
      continue;
    }
    const long long candidate = offsets[grid];
    if (candidate > offset &&
        (nextOffset < 0 || candidate < nextOffset)) {
      nextOffset = candidate;
    }
  }
  return nextOffset;
}

inline bool PathExists(const std::string &path);
inline std::string JoinPath(const std::string &parent,
                            const std::string &child);

inline bool ParseLegacyParticleHeader(const std::string &headerPath,
                                      LegacyParticleHeader &header,
                                      std::string &error) {
  error.clear();
  header = LegacyParticleHeader{};

  std::ifstream in(headerPath);
  if (!in.is_open()) {
    error = "Failed to open particle header '" + headerPath + "'.";
    return false;
  }

  if (!(in >> header.version >> header.spatialDim >> header.numReal
        >> header.numParticles >> header.maxNextId >> header.finestLevel)) {
    error = "Failed to parse legacy particle header '" + headerPath + "'.";
    return false;
  }

  if (header.spatialDim <= 0 || header.numReal < 0 || header.finestLevel < 0) {
    error = "Legacy particle header values out of range in '" + headerPath + "'.";
    return false;
  }

  header.gridsPerLevel.resize(static_cast<size_t>(header.finestLevel + 1));
  for (int lev = 0; lev <= header.finestLevel; ++lev) {
    int ngrids = 0;
    if (!(in >> ngrids)) {
      error = "Failed to read grid count from '" + headerPath + "'.";
      return false;
    }
    header.gridsPerLevel[lev] = ngrids;
  }

  long long totalGrids = 0;
  for (int ngrids : header.gridsPerLevel) {
    totalGrids += ngrids;
  }
  header.gridEntries.reserve(static_cast<size_t>(totalGrids));
  for (long long i = 0; i < totalGrids; ++i) {
    LegacyParticleHeader::GridEntry entry;
    if (!(in >> entry.which >> entry.count >> entry.where)) {
      error = "Failed to read grid entries from '" + headerPath + "'.";
      return false;
    }
    header.gridEntries.push_back(entry);
  }

  return true;
}

inline bool ReadLegacyParticleBlock(const std::string &speciesDir,
                                    const LegacyParticleHeader &header,
                                    int level, int gridIndex,
                                    LegacyParticleBlock &block,
                                    std::string &error) {
  error.clear();
  block = LegacyParticleBlock{};
  block.spatialDim = header.spatialDim;
  block.extraComps = header.numReal;

  if (level < 0 || level > header.finestLevel) {
    error = "Legacy particle level out of range.";
    return false;
  }

  long long offset = 0;
  for (int lev = 0; lev < level; ++lev) {
    offset += header.gridsPerLevel[lev];
  }
  if (gridIndex < 0 || gridIndex >= header.gridsPerLevel[level]) {
    error = "Legacy particle grid index out of range.";
    return false;
  }
  const auto &entry =
      header.gridEntries.at(static_cast<size_t>(offset + gridIndex));
  block.count = entry.count;
  if (block.count <= 0) {
    return true;
  }

  char fileName5[16];
  std::snprintf(fileName5, sizeof(fileName5), "DATA_%05d", entry.which);
  char fileName4[16];
  std::snprintf(fileName4, sizeof(fileName4), "DATA_%04d", entry.which);

  std::string levelDir = JoinPath(speciesDir, "Level_" + std::to_string(level));
  std::string filePath = JoinPath(levelDir, fileName5);
  if (!PathExists(filePath)) {
    std::string altPath = JoinPath(levelDir, fileName4);
    if (PathExists(altPath)) {
      filePath = altPath;
    }
  }
  if (!PathExists(filePath)) {
    error = "Legacy particle data file missing: '" + filePath + "'.";
    return false;
  }

  std::ifstream in(filePath, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    error = "Failed to open legacy particle data file '" + filePath + "'.";
    return false;
  }
  in.seekg(entry.where, std::ios::beg);

  const int totalReals = header.spatialDim + header.numReal;
  if (totalReals <= 0) {
    error = "Legacy particle header has no real components.";
    return false;
  }

  bool isSingle = header.version.find("_single") != std::string::npos;
  const size_t totalValues =
      static_cast<size_t>(block.count) * static_cast<size_t>(totalReals);

  block.positions.resize(static_cast<size_t>(block.count) * 3, 0.0);
  block.extras.resize(static_cast<size_t>(block.count) *
                          static_cast<size_t>(block.extraComps),
                      0.0);

  if (isSingle) {
    std::vector<float> buffer(totalValues);
    in.read(reinterpret_cast<char *>(buffer.data()),
            static_cast<std::streamsize>(sizeof(float) * buffer.size()));
    if (!in) {
      error = "Failed to read legacy particle float data from '" + filePath + "'.";
      return false;
    }
    for (int i = 0; i < block.count; ++i) {
      size_t base = static_cast<size_t>(i) * totalReals;
      for (int axis = 0; axis < header.spatialDim && axis < 3; ++axis) {
        block.positions[i * 3 + axis] =
            static_cast<double>(buffer[base + axis]);
      }
      for (int c = 0; c < block.extraComps; ++c) {
        block.extras[i * block.extraComps + c] =
            static_cast<double>(buffer[base + header.spatialDim + c]);
      }
    }
  } else {
    std::vector<double> buffer(totalValues);
    in.read(reinterpret_cast<char *>(buffer.data()),
            static_cast<std::streamsize>(sizeof(double) * buffer.size()));
    if (!in) {
      error = "Failed to read legacy particle double data from '" + filePath + "'.";
      return false;
    }
    for (int i = 0; i < block.count; ++i) {
      size_t base = static_cast<size_t>(i) * totalReals;
      for (int axis = 0; axis < header.spatialDim && axis < 3; ++axis) {
        block.positions[i * 3 + axis] = buffer[base + axis];
      }
      for (int c = 0; c < block.extraComps; ++c) {
        block.extras[i * block.extraComps + c] =
            buffer[base + header.spatialDim + c];
      }
    }
  }

  return true;
}

inline std::string ParticleDataFileName(const std::string &prefix, int level,
                                        int fileNum) {
  std::ostringstream ss;
  ss << prefix << "/Level_" << level << "/DATA_";
  ss << std::setfill('0') << std::setw(5) << fileNum;
  return ss.str();
}

inline bool ReadParticleBlock(const avtamrexFileFormat::ParticleSpeciesInfo &species,
                              int level, int gridIndex,
                              ParticleBlock &block, std::string &error) {
  error.clear();
  block = ParticleBlock{};

  const auto &header = species.header;
  if (level < 0 || level > header.finestLevel) {
    error = "Particle level out of range.";
    return false;
  }
  if (static_cast<size_t>(level) >= header.fileNums.size() ||
      static_cast<size_t>(level) >= header.particleCounts.size() ||
      static_cast<size_t>(level) >= header.offsets.size()) {
    error = "Particle header missing level metadata.";
    return false;
  }
  if (gridIndex < 0 ||
      gridIndex >= static_cast<int>(header.fileNums[level].size())) {
    error = "Particle grid index out of range.";
    return false;
  }

  const int fileNum = header.fileNums[level][gridIndex];
  const int count = header.particleCounts[level][gridIndex];
  const long long offset = header.offsets[level][gridIndex];
  block.count = count;
  block.numReal = header.numReal;
  block.numInt = header.numInt;
  block.spatialDim = header.spatialDim;
  block.isSingle = header.isSingle;

  if (count <= 0) {
    return true;
  }

  std::string filePath = ParticleDataFileName(species.speciesDir, level, fileNum);
  if (!PathExists(filePath)) {
    std::ostringstream ss;
    ss << species.speciesDir << "/Level_" << level << "/DATA_"
       << std::setfill('0') << std::setw(4) << fileNum;
    std::string altPath = ss.str();
    if (PathExists(altPath)) {
      filePath = altPath;
    }
  }
  if (!PathExists(filePath)) {
    error = "Particle data file missing: '" + filePath + "'.";
    return false;
  }

  std::ifstream in(filePath, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    error = "Failed to open particle data file '" + filePath + "'.";
    return false;
  }
  in.seekg(0, std::ios::end);
  const long long fileSize = static_cast<long long>(in.tellg());
  in.seekg(offset, std::ios::beg);
  if (!in) {
    error = "Failed to seek particle data file '" + filePath + "'.";
    return false;
  }

  const long long nextOffset =
      NextParticleOffsetForFile(header, level, fileNum, offset);
  long long availableBytes = -1;
  if (nextOffset > offset) {
    availableBytes = nextOffset - offset;
  } else if (fileSize >= 0) {
    availableBytes = fileSize - offset;
  }

  const long long realBytes =
      (block.isSingle ? 4LL : 8LL) *
      static_cast<long long>(block.numReal) *
      static_cast<long long>(count);
  if (availableBytes >= 0 && availableBytes < realBytes) {
    error = "Particle data file '" + filePath + "' is truncated.";
    return false;
  }

  int intBytes = static_cast<int>(sizeof(long long));
  if (block.numInt > 0 && availableBytes >= 0) {
    const long long intBytesTotal = availableBytes - realBytes;
    const long long denom =
        static_cast<long long>(block.numInt) * static_cast<long long>(count);
    if (intBytesTotal >= 0 && denom > 0 && intBytesTotal % denom == 0) {
      const long long candidate = intBytesTotal / denom;
      if (candidate == 4 || candidate == 8) {
        intBytes = static_cast<int>(candidate);
      }
    }
  }
  block.intBytes = intBytes;

  if (block.numInt > 0) {
    block.intData.resize(static_cast<size_t>(block.numInt) *
                         static_cast<size_t>(count));
    const size_t intCount =
        static_cast<size_t>(block.numInt) * static_cast<size_t>(count);
    const auto bytes = static_cast<std::streamsize>(
        static_cast<size_t>(block.intBytes) * intCount);
    if (block.intBytes == 8) {
      in.read(reinterpret_cast<char *>(block.intData.data()), bytes);
    } else {
      std::vector<int32_t> tmp(intCount);
      in.read(reinterpret_cast<char *>(tmp.data()), bytes);
      if (in) {
        for (size_t i = 0; i < intCount; ++i) {
          block.intData[i] = static_cast<long long>(tmp[i]);
        }
      }
    }
    if (!in) {
      error = "Failed to read particle integer data from '" + filePath + "'.";
      return false;
    }
  }

  const size_t realCount =
      static_cast<size_t>(block.numReal) * static_cast<size_t>(count);
  if (block.isSingle) {
    block.realDataSingle.resize(realCount);
    const auto bytes = static_cast<std::streamsize>(sizeof(float) * realCount);
    in.read(reinterpret_cast<char *>(block.realDataSingle.data()), bytes);
    if (!in) {
      error = "Failed to read particle float data from '" + filePath + "'.";
      return false;
    }
  } else {
    block.realDataDouble.resize(realCount);
    const auto bytes = static_cast<std::streamsize>(sizeof(double) * realCount);
    in.read(reinterpret_cast<char *>(block.realDataDouble.data()), bytes);
    if (!in) {
      error = "Failed to read particle double data from '" + filePath + "'.";
      return false;
    }
  }

  return true;
}
inline bool IsAbsolutePath(const std::string &path) {
  if (path.empty()) {
    return false;
  }
#ifdef _WIN32
  if (path.size() >= 2 &&
      std::isalpha(static_cast<unsigned char>(path[0])) &&
      path[1] == ':') {
    return true;
  }
  if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') {
    return true;
  }
  return path[0] == '/' || path[0] == '\\';
#else
  return path[0] == '/';
#endif
}

inline std::string ParentDirectory(const std::string &path) {
  if (path.empty()) {
    return "";
  }

  const auto pos = path.find_last_of("/\\");
  if (pos == std::string::npos) {
    return "";
  }

#ifdef _WIN32
  if (pos == 2 && path[1] == ':') {
    return path.substr(0, pos + 1);
  }
#endif
  if (pos == 0) {
    return path.substr(0, 1);
  }
  return path.substr(0, pos);
}

inline bool EndsWithSeparator(const std::string &path) {
  if (path.empty()) {
    return false;
  }
  const char last = path.back();
  return last == '/' || last == '\\';
}

inline std::string TrimTrailingSeparators(const std::string &path) {
  if (path.empty()) {
    return path;
  }
  size_t end = path.size();
  while (end > 1 && IsSeparator(path[end - 1])) {
#ifdef _WIN32
    if (end == 3 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':' && IsSeparator(path[2])) {
      break;
    }
#endif
    --end;
  }
  return path.substr(0, end);
}

inline std::string StripLeadingSeparators(const std::string &path) {
  size_t start = 0;
  while (start < path.size() && IsSeparator(path[start])) {
    ++start;
  }
  return path.substr(start);
}

inline std::string Basename(const std::string &path) {
  if (path.empty()) {
    return "";
  }
  size_t end = path.size();
  while (end > 0 && IsSeparator(path[end - 1])) {
    --end;
  }
  if (end == 0) {
    return "";
  }
  const size_t pos = path.find_last_of("/\\", end - 1);
  if (pos == std::string::npos) {
    return path.substr(0, end);
  }
  return path.substr(pos + 1, end - pos - 1);
}

inline std::string Stem(const std::string &path) {
  std::string base = Basename(path);
  if (base.empty()) {
    return base;
  }
  const size_t dot = base.find_last_of('.');
  if (dot == std::string::npos || dot == 0) {
    return base;
  }
  return base.substr(0, dot);
}

inline bool PathExists(const std::string &path) {
#ifdef _WIN32
  struct _stat st;
  return _stat(path.c_str(), &st) == 0;
#else
  struct stat st;
  return stat(path.c_str(), &st) == 0;
#endif
}

inline bool PathIsDirectory(const std::string &path) {
#ifdef _WIN32
  struct _stat st;
  if (_stat(path.c_str(), &st) != 0) {
    return false;
  }
  return (st.st_mode & _S_IFDIR) != 0;
#else
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    return false;
  }
  return S_ISDIR(st.st_mode);
#endif
}

inline std::string JoinPath(const std::string &parent,
                            const std::string &child) {
  if (child.empty()) {
    return parent;
  }
  if (IsAbsolutePath(child) || parent.empty()) {
    return child;
  }

  std::string result = parent;
  if (!EndsWithSeparator(result)) {
#ifdef _WIN32
    if (result.size() == 2 && result[1] == ':') {
      result.push_back('\\');
    } else {
      result.push_back('/');
    }
#else
    result.push_back('/');
#endif
  }
  result += child;
  return result;
}

inline bool ParsePlotfileDirectoryName(const std::string &name,
                                       std::string &prefix,
                                       unsigned long long &iteration) {
  if (name.empty()) {
    return false;
  }
  size_t end = name.size();
  size_t start = end;
  while (start > 0 &&
         std::isdigit(static_cast<unsigned char>(name[start - 1]))) {
    --start;
  }
  if (start == end) {
    return false;
  }
  prefix = name.substr(0, start);
  std::string digits = name.substr(start);
  try {
    iteration = static_cast<unsigned long long>(std::stoll(digits));
  } catch (std::exception const &) {
    return false;
  }
  return true;
}

inline bool ListDirectoryEntries(const std::string &path,
                                 std::vector<std::string> &entries,
                                 std::string &error) {
  entries.clear();
#ifdef _WIN32
  std::string pattern = JoinPath(path, "*");
  WIN32_FIND_DATAA data;
  HANDLE handle = FindFirstFileA(pattern.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) {
    error = "Failed to list directory '" + path + "'.";
    return false;
  }
  do {
    std::string name = data.cFileName;
    if (name == "." || name == "..") {
      continue;
    }
    entries.push_back(name);
  } while (FindNextFileA(handle, &data));
  FindClose(handle);
  return true;
#else
  errno = 0;
  DIR *dir = opendir(path.c_str());
  if (dir == nullptr) {
    error = "Failed to list directory '" + path + "': " +
            std::string(std::strerror(errno));
    return false;
  }
  while (dirent *entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    entries.push_back(name);
  }
  int saved_errno = errno;
  closedir(dir);
  if (saved_errno != 0) {
    error = "Failed to list directory '" + path + "': " +
            std::string(std::strerror(saved_errno));
    return false;
  }
  return true;
#endif
}

} // namespace

std::vector<std::pair<unsigned long long, std::string>>
avtamrexFileFormat::ResolveDescriptorPaths(const std::string &descriptorPath,
                                             const std::string &rawPath) {
  std::vector<std::pair<unsigned long long, std::string>> matches;
  std::string fullPath = JoinPath(ParentDirectory(descriptorPath), rawPath);
  PathPattern pattern = ParsePattern(fullPath);

  auto pushSingle = [&](const std::string &path) {
    if (!PathExists(path)) {
      std::ostringstream oss;
      oss << "Plotfile '" << path << "' referenced by '" << descriptorPath
          << "' does not exist.";
      debug1 << "[amrex-plugin] " << oss.str() << "\n";
      EXCEPTION1(InvalidFilesException, oss.str().c_str());
    }
    matches.emplace_back(static_cast<unsigned long long>(matches.size()),
                         path);
  };

  if (!pattern.hasPattern) {
    pushSingle(fullPath);
    return matches;
  }

  std::string baseDir = ParentDirectory(pattern.prefix);
  if (baseDir.empty()) {
    baseDir = ".";
  }

  std::string prefixString = pattern.prefix;
  if (!prefixString.empty() && IsSeparator(prefixString.back())) {
    prefixString = TrimTrailingSeparators(prefixString);
    baseDir = prefixString.empty() ? "." : prefixString;
  }

  std::vector<std::string> dirEntries;
  std::string listError;
  if (!ListDirectoryEntries(baseDir, dirEntries, listError)) {
    debug1 << "[amrex-plugin] " << listError << "\n";
    EXCEPTION1(InvalidFilesException, listError.c_str());
  }

  std::string baseDirForJoin = baseDir == "." ? "" : baseDir;
  for (const auto &entryName : dirEntries) {
    std::string entryPathStr = JoinPath(baseDirForJoin, entryName);
    if (entryPathStr.rfind(pattern.prefix, 0) != 0) {
      continue;
    }

    std::string remainder = entryPathStr.substr(pattern.prefix.size());
    if (!remainder.empty() &&
        (IsSeparator(remainder.front()) && !pattern.suffix.empty() &&
         IsSeparator(pattern.suffix.front()))) {
      // Placeholder occupied its own directory component.
      remainder.erase(remainder.begin());
    }

    std::string digits;
    size_t idx = 0;
    while (idx < remainder.size() &&
           std::isdigit(static_cast<unsigned char>(remainder[idx]))) {
      digits.push_back(remainder[idx]);
      ++idx;
    }
    if (digits.empty()) {
      continue;
    }
    if (pattern.width > 0 &&
        digits.size() != static_cast<size_t>(pattern.width)) {
      continue;
    }

    std::string trailing = remainder.substr(idx);
    std::string candidate = entryPathStr;
    if (!pattern.suffix.empty()) {
      if (IsSeparator(pattern.suffix.front())) {
        if (!trailing.empty()) {
          continue;
        }
        candidate = JoinPath(candidate, StripLeadingSeparators(pattern.suffix));
      } else {
        if (trailing != pattern.suffix) {
          continue;
        }
      }
    } else if (!trailing.empty()) {
      continue;
    }

    if (!PathExists(candidate)) {
      continue;
    }

    unsigned long long iterValue = 0;
    try {
      iterValue = static_cast<unsigned long long>(std::stoll(digits));
    } catch (std::exception const &) {
      continue;
    }

    matches.emplace_back(iterValue, candidate);
  }

  std::sort(matches.begin(), matches.end(),
            [](const auto &lhs, const auto &rhs) {
              if (lhs.first != rhs.first) {
                return lhs.first < rhs.first;
              }
              return lhs.second < rhs.second;
            });

  return matches;
}

std::string avtamrexFileFormat::MakeDefaultMeshName(
    const std::string &path) const {
  std::string stem = Stem(path);
  if (stem.empty()) {
    return "amrex_mesh";
  }
  // Remove timestep suffixes to keep metadata stable.
  while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back()))) {
    stem.pop_back();
  }
  if (stem.empty()) {
    stem = "amrex";
  }
  return stem + "_mesh";
}

// ****************************************************************************
//  Method: avtamrexFileFormat constructor
//
//  Programmer: benwibking -- generated by xml2avt
//  Creation:   Fri Dec 6 17:16:49 PST 2024
//
// ****************************************************************************

avtamrexFileFormat::avtamrexFileFormat(const char *filename)
    : avtMTMDFileFormat(filename) {
  InstallSegfaultTraceHandler();
  AmrexRuntime::Retain();
  debug1 << "[amrex-plugin] Constructing reader for plotfile '" << filename
         << "'\n";

  std::string requestedPath = TrimTrailingSeparators(filename);
  if (requestedPath.empty()) {
    requestedPath = filename;
  }
  if (!PathExists(requestedPath)) {
    std::ostringstream oss;
    oss << "Plotfile '" << filename << "' does not exist.";
    debug1 << "[amrex-plugin] " << oss.str() << "\n";
    EXCEPTION1(InvalidFilesException, oss.str().c_str());
  }

  if (!PathIsDirectory(requestedPath)) {
    const std::string baseName = Basename(requestedPath);
    if (baseName == "Header") {
      std::string parentPath = ParentDirectory(requestedPath);
      if (parentPath.empty() || !PathIsDirectory(parentPath)) {
        std::ostringstream oss;
        oss << "Plotfile header '" << filename
            << "' does not reside in a plotfile directory.";
        debug1 << "[amrex-plugin] " << oss.str() << "\n";
        EXCEPTION1(InvalidFilesException, oss.str().c_str());
      }
      requestedPath = parentPath;
    } else {
      std::ostringstream oss;
      oss << "Plotfile '" << filename
          << "' is not a plotfile directory or Header file.";
      debug1 << "[amrex-plugin] " << oss.str() << "\n";
      EXCEPTION1(InvalidFilesException, oss.str().c_str());
    }
  }

  const std::string baseName = Basename(requestedPath);
  unsigned long long requestedIter = 0;
  std::string requestedPrefix;
  if (!ParsePlotfileDirectoryName(baseName, requestedPrefix, requestedIter)) {
    std::ostringstream oss;
    oss << "Plotfile '" << filename
        << "' does not end with a numeric timestep suffix.";
    debug1 << "[amrex-plugin] " << oss.str() << "\n";
    EXCEPTION1(InvalidFilesException, oss.str().c_str());
  }

  std::string baseDir = ParentDirectory(requestedPath);
  if (baseDir.empty()) {
    baseDir = ".";
  }

  std::vector<std::string> dirEntries;
  std::string listError;
  if (!ListDirectoryEntries(baseDir, dirEntries, listError)) {
    debug1 << "[amrex-plugin] " << listError << "\n";
    EXCEPTION1(InvalidFilesException, listError.c_str());
  }

  std::vector<std::pair<unsigned long long, std::string>> matches;
  std::string baseDirForJoin = baseDir == "." ? "" : baseDir;
  for (const auto &entryName : dirEntries) {
    unsigned long long iterValue = 0;
    std::string entryPrefix;
    if (!ParsePlotfileDirectoryName(entryName, entryPrefix, iterValue)) {
      continue;
    }
    if (entryPrefix != requestedPrefix) {
      continue;
    }

    std::string entryPath = JoinPath(baseDirForJoin, entryName);
    if (!PathIsDirectory(entryPath)) {
      continue;
    }
    matches.emplace_back(iterValue, entryPath);
  }

  if (matches.empty()) {
    std::ostringstream oss;
    oss << "No plotfile directories matching '" << requestedPrefix
        << "<digits>' found in '" << baseDir << "'.";
    debug1 << "[amrex-plugin] " << oss.str() << "\n";
    EXCEPTION1(InvalidFilesException, oss.str().c_str());
  }

  std::sort(matches.begin(), matches.end(),
            [](const auto &lhs, const auto &rhs) {
              if (lhs.first != rhs.first) {
                return lhs.first < rhs.first;
              }
              return lhs.second < rhs.second;
            });

  plotfilePaths_.reserve(matches.size());
  iterationIndex_.reserve(matches.size());
  for (const auto &entry : matches) {
    iterationIndex_.push_back(entry.first);
    plotfilePaths_.push_back(entry.second);
  }

  const size_t numTimesteps = plotfilePaths_.size();
  meshHierarchyCache_.resize(numTimesteps);
  particleSpeciesCache_.resize(numTimesteps);
  plotfileCache_.resize(numTimesteps);
  plotfileImplCache_.resize(numTimesteps);
  timeValues_.assign(numTimesteps, std::numeric_limits<double>::quiet_NaN());

  debug1 << "[amrex-plugin] Constructor ready. plotfile='" << filename
         << "' timesteps=" << numTimesteps << "\n";
}

avtamrexFileFormat::~avtamrexFileFormat() {
  AmrexRuntime::Release();
  debug1 << "[amrex-plugin] Destructor invoked. this="
         << static_cast<const void *>(this) << "\n";
}

std::shared_ptr<amrex::PlotFileData>
avtamrexFileFormat::GetPlotFile(int timeState) const {
  if (timeState < 0 ||
      timeState >= static_cast<int>(plotfilePaths_.size())) {
    EXCEPTION1(InvalidVariableException, "Timestep out of range");
  }

  auto &cache = plotfileCache_.at(static_cast<size_t>(timeState));
  if (!cache) {
    cache = std::make_shared<amrex::PlotFileData>(plotfilePaths_[timeState]);
    timeValues_[timeState] = cache->time();
  }
  return cache;
}

std::shared_ptr<amrex::PlotFileDataImpl>
avtamrexFileFormat::GetPlotFileImpl(int timeState) const {
  if (timeState < 0 ||
      timeState >= static_cast<int>(plotfilePaths_.size())) {
    EXCEPTION1(InvalidVariableException, "Timestep out of range");
  }

  auto &cache = plotfileImplCache_.at(static_cast<size_t>(timeState));
  if (!cache) {
    cache = std::make_shared<amrex::PlotFileDataImpl>(plotfilePaths_[timeState]);
    timeValues_[timeState] = cache->time();
  }
  return cache;
}

std::shared_ptr<amrex::VisMF>
avtamrexFileFormat::GetVisMF(int timeState, int level) const {
  VisMFCacheKey key{timeState, level};
  auto it = vismfCache_.find(key);
  if (it != vismfCache_.end()) {
    return it->second;
  }

  auto plotfileImpl = GetPlotFileImpl(timeState);
  if (plotfileImpl == nullptr) {
    EXCEPTION1(InvalidFilesException, plotfilePaths_[timeState].c_str());
  }
  if (level < 0 ||
      level >= static_cast<int>(plotfileImpl->m_vismf.size())) {
    debug1 << "[amrex-plugin] GetVisMF invalid level " << level
           << " with levels " << plotfileImpl->m_vismf.size() << "\n";
    EXCEPTION1(InvalidVariableException, "Level out of range");
  }

  auto &vismfPtr = plotfileImpl->m_vismf[level];
  if (!vismfPtr) {
    std::string mfName = plotfileImpl->m_mf_name[level];
    if (!mfName.empty() && mfName[0] != '/') {
      mfName = plotfilePaths_[timeState] + "/" + mfName;
    }
    debug1 << "[amrex-plugin] GetVisMF constructing VisMF timeState="
           << timeState << " level=" << level << " mfName='" << mfName
           << "'\n";
    vismfPtr = std::make_unique<amrex::VisMF>(mfName);
  }

  auto vismf = std::shared_ptr<amrex::VisMF>(plotfileImpl, vismfPtr.get());
  debug1 << "[amrex-plugin] GetVisMF ready level=" << level
         << " size=" << vismf->size() << " nComp=" << vismf->nComp() << "\n";
  vismfCache_.emplace(key, vismf);
  return vismf;
}

vtkDataSet *avtamrexFileFormat::GetParticleMesh(
    int timeState, int domain, const ParticleSpeciesInfo &species,
    const MeshPatchHierarchy &hierarchy) const {
  if (domain < 0 || domain >= static_cast<int>(hierarchy.patches.size())) {
    EXCEPTION1(InvalidVariableException, species.meshName.c_str());
  }

  const PatchInfo &patch = hierarchy.patches.at(domain);
  debug2 << "[amrex-plugin] GetParticleMesh species='"
         << species.speciesName << "' domain=" << domain
         << " level=" << patch.level
         << " fabIndex=" << patch.fabIndex
         << " legacy=" << species.legacyHeader << "\n";
  if (species.legacyHeader) {
    LegacyParticleHeader legacy;
    std::string parseError;
    std::string headerPath =
        JoinPath(JoinPath(plotfilePaths_[timeState], species.speciesName),
                 "Header");
    if (!ParseLegacyParticleHeader(headerPath, legacy, parseError)) {
      debug1 << "[amrex-plugin] Failed to parse legacy particle header '"
             << headerPath << "': " << parseError << "\n";
      EXCEPTION1(InvalidFilesException, headerPath.c_str());
    }

    LegacyParticleBlock block;
    std::string readError;
    std::string speciesDir =
        JoinPath(plotfilePaths_[timeState], species.speciesName);
    if (!ReadLegacyParticleBlock(speciesDir, legacy, patch.level,
                                 patch.fabIndex, block, readError)) {
      debug1 << "[amrex-plugin] " << readError << "\n";
      EXCEPTION1(InvalidFilesException, species.meshName.c_str());
    }

    vtkNew<vtkPoints> points;
    vtkNew<vtkDoubleArray> coords;
    coords->SetNumberOfComponents(3);
    coords->SetNumberOfTuples(block.count);
    double *buffer = coords->GetPointer(0);
    for (int i = 0; i < block.count; ++i) {
      vtkIdType base = static_cast<vtkIdType>(i) * 3;
      buffer[base] = block.positions[i * 3];
      buffer[base + 1] = block.positions[i * 3 + 1];
      buffer[base + 2] = block.positions[i * 3 + 2];
    }
    points->SetData(coords);

    vtkNew<vtkPolyData> poly;
    poly->SetPoints(points);
    vtkNew<vtkCellArray> verts;
    if (block.count > 0) {
      verts->AllocateExact(block.count, 1);
      for (vtkIdType i = 0; i < block.count; ++i) {
        verts->InsertNextCell(1);
        verts->InsertCellPoint(i);
      }
    }
    poly->SetVerts(verts);
    vtkPolyData *dataset = poly.GetPointer();
    dataset->Register(nullptr);
    return dataset;
  }
  ParticleBlock block;
  std::string readError;
  if (!ReadParticleBlock(species, patch.level, patch.fabIndex, block, readError)) {
    debug1 << "[amrex-plugin] " << readError << "\n";
    EXCEPTION1(InvalidFilesException, species.meshName.c_str());
  }

  vtkNew<vtkPoints> points;
  vtkSmartPointer<vtkDataArray> coords;
  if (block.isSingle) {
    vtkNew<vtkFloatArray> arr;
    coords = arr;
  } else {
    vtkNew<vtkDoubleArray> arr;
    coords = arr;
  }
  coords->SetNumberOfComponents(3);
  coords->SetNumberOfTuples(block.count);

  const int spatialDim = block.spatialDim > 0
                             ? std::min(block.spatialDim, AMREX_SPACEDIM)
                             : AMREX_SPACEDIM;
  if (block.count > 0) {
    if (coords->GetDataType() == VTK_FLOAT) {
      float *buffer = static_cast<float *>(coords->GetVoidPointer(0));
      const float *reals = block.realDataSingle.data();
      for (int i = 0; i < block.count; ++i) {
        const int base = i * block.numReal;
        vtkIdType out = static_cast<vtkIdType>(i) * 3;
        buffer[out] = reals[base];
        buffer[out + 1] = (spatialDim > 1) ? reals[base + 1] : 0.0f;
        buffer[out + 2] = (spatialDim > 2) ? reals[base + 2] : 0.0f;
      }
    } else {
      double *buffer = static_cast<double *>(coords->GetVoidPointer(0));
      const double *reals = block.realDataDouble.data();
      for (int i = 0; i < block.count; ++i) {
        const int base = i * block.numReal;
        vtkIdType out = static_cast<vtkIdType>(i) * 3;
        buffer[out] = reals[base];
        buffer[out + 1] = (spatialDim > 1) ? reals[base + 1] : 0.0;
        buffer[out + 2] = (spatialDim > 2) ? reals[base + 2] : 0.0;
      }
    }
  }

  points->SetData(coords);

  vtkNew<vtkPolyData> poly;
  poly->SetPoints(points);

  vtkNew<vtkCellArray> verts;
  if (block.count > 0) {
    verts->AllocateExact(block.count, 1);
    for (vtkIdType i = 0; i < block.count; ++i) {
      verts->InsertNextCell(1);
      verts->InsertCellPoint(i);
    }
  }
  poly->SetVerts(verts);
  vtkPolyData *dataset = poly.GetPointer();
  dataset->Register(nullptr);
  return dataset;
}

vtkDataArray *avtamrexFileFormat::GetParticleVar(
    int timeState, int domain, const ParticleVarInfo &varInfo,
    const MeshPatchHierarchy &hierarchy) const {
  if (domain < 0 || domain >= static_cast<int>(hierarchy.patches.size())) {
    EXCEPTION1(InvalidVariableException, varInfo.meshName.c_str());
  }

  const PatchInfo &patch = hierarchy.patches.at(domain);
  debug2 << "[amrex-plugin] GetParticleVar var='"
         << varInfo.meshName << "' species='" << varInfo.speciesName
         << "' domain=" << domain
         << " level=" << patch.level
         << " fabIndex=" << patch.fabIndex
         << " isReal=" << varInfo.isReal
         << " comp=" << varInfo.componentIndex << "\n";
  auto &speciesMap = particleSpeciesCache_.at(timeState);
  auto speciesIt = speciesMap.find(varInfo.speciesName);
  if (speciesIt != speciesMap.end() && speciesIt->second.legacyHeader) {
    if (!varInfo.isReal) {
      EXCEPTION1(InvalidVariableException, varInfo.meshName.c_str());
    }
    LegacyParticleHeader legacy;
    std::string parseError;
    std::string headerPath =
        JoinPath(JoinPath(plotfilePaths_[timeState], varInfo.speciesName),
                 "Header");
    if (!ParseLegacyParticleHeader(headerPath, legacy, parseError)) {
      debug1 << "[amrex-plugin] Failed to parse legacy particle header '"
             << headerPath << "': " << parseError << "\n";
      EXCEPTION1(InvalidFilesException, headerPath.c_str());
    }

    LegacyParticleBlock block;
    std::string readError;
    std::string speciesDir =
        JoinPath(plotfilePaths_[timeState], varInfo.speciesName);
    if (!ReadLegacyParticleBlock(speciesDir, legacy, patch.level,
                                 patch.fabIndex, block, readError)) {
      debug1 << "[amrex-plugin] " << readError << "\n";
      EXCEPTION1(InvalidFilesException, varInfo.meshName.c_str());
    }
    if (varInfo.componentIndex < 0 ||
        varInfo.componentIndex >= block.extraComps) {
      EXCEPTION1(InvalidVariableException, varInfo.meshName.c_str());
    }

    vtkDoubleArray *array = vtkDoubleArray::New();
    array->SetNumberOfComponents(1);
    array->SetNumberOfTuples(block.count);
    double *buffer = array->GetPointer(0);
    for (int i = 0; i < block.count; ++i) {
      buffer[i] = block.extras[i * block.extraComps + varInfo.componentIndex];
    }
    return array;
  }
  if (speciesIt == speciesMap.end()) {
    EXCEPTION1(InvalidVariableException, varInfo.meshName.c_str());
  }

  ParticleBlock block;
  std::string readError;
  if (!ReadParticleBlock(speciesIt->second, patch.level, patch.fabIndex, block,
                         readError)) {
    debug1 << "[amrex-plugin] " << readError << "\n";
    EXCEPTION1(InvalidFilesException, varInfo.meshName.c_str());
  }
  if (varInfo.isReal) {
    if (varInfo.componentIndex < 0 ||
        varInfo.componentIndex >= block.numReal) {
      EXCEPTION1(InvalidVariableException, varInfo.meshName.c_str());
    }
  } else {
    if (varInfo.componentIndex < 0 ||
        varInfo.componentIndex >= block.numInt) {
      EXCEPTION1(InvalidVariableException, varInfo.meshName.c_str());
    }
  }

  vtkSmartPointer<vtkDataArray> array;
  if (varInfo.isReal) {
    if (block.isSingle) {
      vtkNew<vtkFloatArray> arr;
      array = arr;
    } else {
      vtkNew<vtkDoubleArray> arr;
      array = arr;
    }
  } else {
    if (block.intBytes == 8) {
      vtkNew<vtkLongLongArray> arr;
      array = arr;
    } else {
      vtkNew<vtkIntArray> arr;
      array = arr;
    }
  }

  array->SetNumberOfComponents(1);
  array->SetNumberOfTuples(block.count);

  if (block.count > 0) {
    if (varInfo.isReal) {
      if (array->GetDataType() == VTK_FLOAT) {
        float *buffer = static_cast<float *>(array->GetVoidPointer(0));
        const float *reals = block.realDataSingle.data();
        for (int i = 0; i < block.count; ++i) {
          buffer[i] = reals[i * block.numReal + varInfo.componentIndex];
        }
      } else {
        double *buffer = static_cast<double *>(array->GetVoidPointer(0));
        const double *reals = block.realDataDouble.data();
        for (int i = 0; i < block.count; ++i) {
          buffer[i] = reals[i * block.numReal + varInfo.componentIndex];
        }
      }
    } else {
      if (array->GetDataType() == VTK_LONG_LONG) {
        auto *buffer = static_cast<long long *>(array->GetVoidPointer(0));
        const long long *ints = block.intData.data();
        for (int i = 0; i < block.count; ++i) {
          buffer[i] = ints[i * block.numInt + varInfo.componentIndex];
        }
      } else {
        int *buffer = static_cast<int *>(array->GetVoidPointer(0));
        const long long *ints = block.intData.data();
        for (int i = 0; i < block.count; ++i) {
          buffer[i] = static_cast<int>(
              ints[i * block.numInt + varInfo.componentIndex]);
        }
      }
    }
  }

  array->Register(nullptr);
  return array;
}

vtkDataArray *avtamrexFileFormat::GetParticleVectorVar(
    int timeState, int domain, const ParticleVectorVarInfo &varInfo,
    const MeshPatchHierarchy &hierarchy) const {
  if (domain < 0 || domain >= static_cast<int>(hierarchy.patches.size())) {
    EXCEPTION1(InvalidVariableException, varInfo.meshName.c_str());
  }

  const PatchInfo &patch = hierarchy.patches.at(domain);
  std::ostringstream comps;
  for (size_t idx = 0; idx < varInfo.componentIndices.size(); ++idx) {
    if (idx > 0) {
      comps << ", ";
    }
    comps << varInfo.componentIndices[idx];
  }
  debug2 << "[amrex-plugin] GetParticleVectorVar var='"
         << varInfo.meshName << "' species='" << varInfo.speciesName
         << "' domain=" << domain
         << " level=" << patch.level
         << " fabIndex=" << patch.fabIndex
         << " isReal=" << varInfo.isReal
         << " comps=[" << comps.str() << "]\n";
  auto &speciesMap = particleSpeciesCache_.at(timeState);
  auto speciesIt = speciesMap.find(varInfo.speciesName);
  if (speciesIt != speciesMap.end() && speciesIt->second.legacyHeader) {
    if (!varInfo.isReal) {
      EXCEPTION1(InvalidVariableException, varInfo.meshName.c_str());
    }
    LegacyParticleHeader legacy;
    std::string parseError;
    std::string headerPath =
        JoinPath(JoinPath(plotfilePaths_[timeState], varInfo.speciesName),
                 "Header");
    if (!ParseLegacyParticleHeader(headerPath, legacy, parseError)) {
      debug1 << "[amrex-plugin] Failed to parse legacy particle header '"
             << headerPath << "': " << parseError << "\n";
      EXCEPTION1(InvalidFilesException, headerPath.c_str());
    }

    LegacyParticleBlock block;
    std::string readError;
    std::string speciesDir =
        JoinPath(plotfilePaths_[timeState], varInfo.speciesName);
    if (!ReadLegacyParticleBlock(speciesDir, legacy, patch.level,
                                 patch.fabIndex, block, readError)) {
      debug1 << "[amrex-plugin] " << readError << "\n";
      EXCEPTION1(InvalidFilesException, varInfo.meshName.c_str());
    }

    int numComponents = static_cast<int>(varInfo.componentIndices.size());
    vtkDoubleArray *array = vtkDoubleArray::New();
    array->SetNumberOfComponents(numComponents);
    array->SetNumberOfTuples(block.count);
    double *buffer = array->GetPointer(0);
    for (int i = 0; i < block.count; ++i) {
      vtkIdType base = static_cast<vtkIdType>(i) * numComponents;
      for (int c = 0; c < numComponents; ++c) {
        int comp = varInfo.componentIndices[c];
        if (comp < 0 || comp >= block.spatialDim) {
          buffer[base + c] = 0.0;
        } else {
          buffer[base + c] = block.positions[i * 3 + comp];
        }
      }
    }
    return array;
  }
  if (speciesIt == speciesMap.end()) {
    EXCEPTION1(InvalidVariableException, varInfo.meshName.c_str());
  }

  ParticleBlock block;
  std::string readError;
  if (!ReadParticleBlock(speciesIt->second, patch.level, patch.fabIndex, block,
                         readError)) {
    debug1 << "[amrex-plugin] " << readError << "\n";
    EXCEPTION1(InvalidFilesException, varInfo.meshName.c_str());
  }
  if (varInfo.isReal) {
    for (int comp : varInfo.componentIndices) {
      if (comp < 0 || comp >= block.numReal) {
        EXCEPTION1(InvalidVariableException, varInfo.meshName.c_str());
      }
    }
  } else {
    for (int comp : varInfo.componentIndices) {
      if (comp < 0 || comp >= block.numInt) {
        EXCEPTION1(InvalidVariableException, varInfo.meshName.c_str());
      }
    }
  }

  vtkSmartPointer<vtkDataArray> array;
  if (varInfo.isReal) {
    if (block.isSingle) {
      vtkNew<vtkFloatArray> arr;
      array = arr;
    } else {
      vtkNew<vtkDoubleArray> arr;
      array = arr;
    }
  } else {
    if (block.intBytes == 8) {
      vtkNew<vtkLongLongArray> arr;
      array = arr;
    } else {
      vtkNew<vtkIntArray> arr;
      array = arr;
    }
  }

  const int numComponents = static_cast<int>(varInfo.componentIndices.size());
  array->SetNumberOfComponents(numComponents);
  array->SetNumberOfTuples(block.count);

  if (block.count > 0) {
    if (varInfo.isReal) {
      if (array->GetDataType() == VTK_FLOAT) {
        float *buffer = static_cast<float *>(array->GetVoidPointer(0));
        const float *reals = block.realDataSingle.data();
        for (int i = 0; i < block.count; ++i) {
          vtkIdType base = static_cast<vtkIdType>(i) * numComponents;
          const int src = i * block.numReal;
          for (int c = 0; c < numComponents; ++c) {
            buffer[base + c] =
                reals[src + varInfo.componentIndices[c]];
          }
        }
      } else {
        double *buffer = static_cast<double *>(array->GetVoidPointer(0));
        const double *reals = block.realDataDouble.data();
        for (int i = 0; i < block.count; ++i) {
          vtkIdType base = static_cast<vtkIdType>(i) * numComponents;
          const int src = i * block.numReal;
          for (int c = 0; c < numComponents; ++c) {
            buffer[base + c] =
                reals[src + varInfo.componentIndices[c]];
          }
        }
      }
    } else {
      if (array->GetDataType() == VTK_LONG_LONG) {
        auto *buffer = static_cast<long long *>(array->GetVoidPointer(0));
        const long long *ints = block.intData.data();
        for (int i = 0; i < block.count; ++i) {
          vtkIdType base = static_cast<vtkIdType>(i) * numComponents;
          const int src = i * block.numInt;
          for (int c = 0; c < numComponents; ++c) {
            buffer[base + c] = ints[src + varInfo.componentIndices[c]];
          }
        }
      } else {
        int *buffer = static_cast<int *>(array->GetVoidPointer(0));
        const long long *ints = block.intData.data();
        for (int i = 0; i < block.count; ++i) {
          vtkIdType base = static_cast<vtkIdType>(i) * numComponents;
          const int src = i * block.numInt;
          for (int c = 0; c < numComponents; ++c) {
            buffer[base + c] = static_cast<int>(
                ints[src + varInfo.componentIndices[c]]);
          }
        }
      }
    }
  }

  array->Register(nullptr);
  return array;
}

void avtamrexFileFormat::QueueVisMFClear(const VisMFCacheKey &key, int fabIndex,
                                         int compIndex) const {
  vismfClearList_.push_back({key, fabIndex, compIndex});
}

std::string avtamrexFileFormat::GetMultiFabName(int timeState,
                                                int level) const {
  const auto key = std::make_pair(timeState, level);
  auto cached = mfNameCache_.find(key);
  if (cached != mfNameCache_.end()) {
    return cached->second;
  }

  if (timeState < 0 ||
      timeState >= static_cast<int>(plotfilePaths_.size())) {
    EXCEPTION1(InvalidVariableException, "Timestep out of range");
  }

  auto plotfile = GetPlotFileImpl(timeState);
  if (plotfile == nullptr) {
    EXCEPTION1(InvalidFilesException, plotfilePaths_[timeState].c_str());
  }

  int finestLevel = plotfile->finestLevel();
  if (level < 0 || level > finestLevel) {
    debug1 << "[amrex-plugin] MultiFab header read invalid level " << level
           << " with finest level " << finestLevel << "\n";
    EXCEPTION1(InvalidVariableException, "Level out of range");
  }

  if (level >= static_cast<int>(plotfile->m_mf_name.size()) ||
      plotfile->m_mf_name[level].empty()) {
    debug1 << "[amrex-plugin] Missing MultiFab name for level " << level
           << " in plotfile '" << plotfilePaths_[timeState] << "'\n";
    EXCEPTION1(InvalidFilesException, plotfilePaths_[timeState].c_str());
  }

  std::string mfName = plotfile->m_mf_name[level];
  if (!mfName.empty() && mfName[0] != '/') {
    // Plotfile headers store relative MultiFab paths; make them absolute
    // so parallel engines don't depend on the current working directory.
    mfName = plotfilePaths_[timeState] + "/" + mfName;
  }
  mfNameCache_.emplace(key, mfName);
  return mfName;
}

// ****************************************************************************
//  Method: avtamrexFileFormat::GetNTimesteps
//
//  Purpose:
//      Tells the rest of the code how many timesteps there are in this file.
//
//  Programmer: benwibking -- generated by xml2avt
//  Creation:   Fri Dec 6 17:16:49 PST 2024
//
// ****************************************************************************

int avtamrexFileFormat::GetNTimesteps(void) {
  return static_cast<int>(plotfilePaths_.size());
}

// ****************************************************************************
//  Method: avtamrexFileFormat::FreeUpResources
//
//  Purpose:
//      When VisIt is done focusing on a particular timestep, it asks that
//      timestep to free up any resources (memory, file descriptors) that
//      it has associated with it.  This method is the mechanism for doing
//      that.
//
//  Programmer: benwibking -- generated by xml2avt
//  Creation:   Fri Dec 6 17:16:49 PST 2024
//
// ****************************************************************************

void avtamrexFileFormat::FreeUpResources(void) {
  debug1 << "[amrex-plugin] FreeUpResources\n";
  for (auto &entry : plotfileCache_) {
    entry.reset();
  }
  for (auto &entry : plotfileImplCache_) {
    entry.reset();
  }

  particleVarMap_.clear();
  particleVectorVarMap_.clear();

  if (!vismfClearList_.empty()) {
    std::sort(vismfClearList_.begin(), vismfClearList_.end());
    auto last = std::unique(vismfClearList_.begin(), vismfClearList_.end(),
                            [](const VisMFClearEntry &lhs,
                               const VisMFClearEntry &rhs) {
                              return lhs.key.timeState == rhs.key.timeState &&
                                     lhs.key.level == rhs.key.level &&
                                     lhs.fabIndex == rhs.fabIndex &&
                                     lhs.compIndex == rhs.compIndex;
                            });
    vismfClearList_.erase(last, vismfClearList_.end());

    for (const auto &entry : vismfClearList_) {
      auto it = vismfCache_.find(entry.key);
      if (it != vismfCache_.end() && it->second != nullptr) {
        it->second->clear(entry.fabIndex, entry.compIndex);
      }
    }
    vismfClearList_.clear();
  }
  vismfCache_.clear();
}

void avtamrexFileFormat::PopulateHierarchyCache(int timeState) {
  auto &hierarchyMap = meshHierarchyCache_.at(timeState);
  hierarchyMap.clear();

  std::string meshName = MakeDefaultMeshName(plotfilePaths_[timeState]);
  MeshPatchHierarchy hierarchy =
      BuildHierarchyFromPlotfile(meshName, *GetPlotFile(timeState));
  hierarchy.metadataInitialized = true;
  hierarchyMap[meshName] = hierarchy;

#if !defined(AMREX_DISABLE_STRUCTURED_BOUNDARY_CACHE)
  if (cache != nullptr) {
    avtStructuredDomainBoundaries *structured =
        BuildStructuredDomainBoundaries(hierarchy);
    if (structured != nullptr) {
      void_ref_ptr vr(structured, avtStructuredDomainBoundaries::Destruct);
      cache->CacheVoidRef(meshName.c_str(),
                          AUXILIARY_DATA_DOMAIN_BOUNDARY_INFORMATION,
                          timeState, -1, vr);
      cache->CacheVoidRef("any_mesh",
                          AUXILIARY_DATA_DOMAIN_BOUNDARY_INFORMATION,
                          timeState, -1, vr);
      debug1 << "[amrex-plugin] Cached structured boundaries for mesh '"
             << meshName << "' timeState=" << timeState << "\n";
    }
  }
#else
  debug2 << "[amrex-plugin] Skipping structured boundary cache for mesh '"
         << meshName << "' due to AMREX_DISABLE_STRUCTURED_BOUNDARY_CACHE"
         << "\n";
#endif

  meshMap_[meshName] = std::tuple(DatasetType::Field, meshName);
  debug2 << "[amrex-plugin] Registered AMR mesh '" << meshName
         << "' patches=" << hierarchy.patches.size()
         << " levels=" << hierarchy.numLevels << "\n";

}


void avtamrexFileFormat::BuildFieldHierarchy(avtDatabaseMetaData *md,
                                               amrex::PlotFileData &plotfile,
                                               int timeState) {
  if (timeState < 0 ||
      timeState >= static_cast<int>(plotfilePaths_.size())) {
    EXCEPTION1(InvalidVariableException, "timestep out of range");
  }

  std::string meshName = MakeDefaultMeshName(plotfilePaths_.at(timeState));
  auto &hierarchyMap = meshHierarchyCache_.at(timeState);
  auto it = hierarchyMap.find(meshName);
  if (it == hierarchyMap.end()) {
    debug1 << "[amrex-plugin] BuildFieldHierarchy missing hierarchy for mesh '"
           << meshName << "'\n";
    EXCEPTION1(InvalidVariableException, meshName.c_str());
  }

  const MeshPatchHierarchy &hierarchy = it->second;

  if (md != nullptr) {
    avtMeshMetaData *meshMd = new avtMeshMetaData;
    meshMd->name = meshName;
    meshMd->meshType = AVT_AMR_MESH;
    meshMd->topologicalDimension = hierarchy.topologicalDim;
    meshMd->spatialDimension = hierarchy.spatialDim;
    meshMd->numBlocks = static_cast<int>(hierarchy.patches.size());
    meshMd->blockTitle = "patches";
    meshMd->blockPieceName = "patch";
    meshMd->numGroups = hierarchy.numLevels;
    meshMd->groupTitle = "levels";
    meshMd->groupPieceName = "level";
    meshMd->blockNames = hierarchy.blockNames;
    meshMd->containsGhostZones = AVT_HAS_GHOSTS;
    meshMd->presentGhostZoneTypes = (1 << AVT_NESTING_GHOST_ZONES);
    md->Add(meshMd);
    intVector blockIds = hierarchy.groupIds;
    md->AddGroupInformation(hierarchy.numLevels,
                            static_cast<int>(hierarchy.patches.size()),
                            blockIds);
  }

  RegisterFieldVariables(md, meshName, plotfile);

  debug1 << "[amrex-plugin] Field hierarchy ready. meshes="
         << hierarchyMap.size() << " vars=" << varMap_.size()
         << " vectors=" << vectorVarMap_.size() << "\n";
}

void avtamrexFileFormat::BuildParticleHierarchy(avtDatabaseMetaData *md,
                                                amrex::PlotFileData &plotfile,
                                                int timeState) {
  if (timeState < 0 ||
      timeState >= static_cast<int>(plotfilePaths_.size())) {
    EXCEPTION1(InvalidVariableException, "timestep out of range");
  }

  std::string baseMeshName = MakeDefaultMeshName(plotfilePaths_.at(timeState));
  auto &hierarchyMap = meshHierarchyCache_.at(timeState);
  auto hierarchyIt = hierarchyMap.find(baseMeshName);
  if (hierarchyIt == hierarchyMap.end()) {
    debug1 << "[amrex-plugin] BuildParticleHierarchy missing hierarchy for mesh '"
           << baseMeshName << "'\n";
    return;
  }

  auto &speciesCache = particleSpeciesCache_.at(timeState);
  speciesCache.clear();

  std::vector<std::string> entries;
  std::string listError;
  if (!ListDirectoryEntries(plotfilePaths_[timeState], entries, listError)) {
    debug1 << "[amrex-plugin] " << listError << "\n";
    return;
  }
  debug2 << "[amrex-plugin] Particle scan dir=" << plotfilePaths_[timeState]
         << " entries=" << JoinStrings(entries) << "\n";
  debug2 << "[amrex-plugin] Particle scan count=" << entries.size()
         << " timeState=" << timeState << "\n";

  auto buildParticleHierarchy =
      [&](const std::string &meshName, int spatialDim,
          const std::vector<int> &numGrids) -> MeshPatchHierarchy {
        MeshPatchHierarchy hierarchy;
        hierarchy.numLevels = static_cast<int>(numGrids.size());
        hierarchy.topologicalDim = 0;
        hierarchy.spatialDim = spatialDim;
        hierarchy.levelValues.resize(hierarchy.numLevels);
        hierarchy.patchesPerLevel.assign(hierarchy.numLevels, {});
        hierarchy.levelCellSizes.assign(hierarchy.numLevels,
                                        std::array<double, 3>{0.0, 0.0, 0.0});
        if (hierarchy.numLevels > 1) {
          hierarchy.levelRefinementRatios.assign(
              hierarchy.numLevels - 1, std::array<int, 3>{1, 1, 1});
        }

        for (int level = 0; level < hierarchy.numLevels; ++level) {
          hierarchy.levelValues[level] = level;
          const int grids = numGrids[static_cast<size_t>(level)];
          hierarchy.patchesPerLevel[level].reserve(
              static_cast<size_t>(std::max(0, grids)));
          for (int grid = 0; grid < grids; ++grid) {
            PatchInfo patch;
            patch.meshName = meshName;
            patch.level = level;
            patch.fabIndex = grid;
            patch.centering = AVT_NODECENT;
            patch.spatialDim = spatialDim;
            patch.offset.assign(3, 0);
            patch.extent.assign(3, 0);
            patch.storageOffset.assign(3, 0);
            patch.storageExtent.assign(3, 0);
            patch.storageOffsetCanonical.assign(3, 0);
            patch.storageExtentCanonical.assign(3, 0);

            hierarchy.patches.push_back(patch);
            int patchIndex = static_cast<int>(hierarchy.patches.size()) - 1;
            hierarchy.levelIdsPerPatch.push_back(level);
            hierarchy.groupIds.push_back(level);
            hierarchy.patchesPerLevel[level].push_back(patchIndex);

            std::ostringstream blockName;
            blockName << "level" << level << ",grid" << grid;
            hierarchy.blockNames.push_back(blockName.str());
          }
        }

        hierarchy.metadataInitialized = true;
        return hierarchy;
      };

  for (const auto &entry : entries) {
    std::string entryPath = JoinPath(plotfilePaths_[timeState], entry);
    if (!PathIsDirectory(entryPath)) {
      debug5 << "[amrex-plugin] Particle candidate '" << entry
             << "' not a directory (" << entryPath << ")\n";
      continue;
    }

    std::string headerPath = JoinPath(entryPath, "Header");
    if (!PathExists(headerPath)) {
      debug5 << "[amrex-plugin] Particle candidate '" << entry
             << "' missing Header at " << headerPath << "\n";
      continue;
    }
    debug5 << "[amrex-plugin] Particle candidate '" << entry
           << "' header=" << headerPath << "\n";

    avtamrexFileFormat::ParticleHeaderInfo header;
    std::string parseError;
    if (!ParseParticleHeader(headerPath, header, parseError)) {
      if (!parseError.empty()) {
        debug2 << "[amrex-plugin] Skipping particle header '"
               << headerPath << "': " << parseError << "\n";
      } else {
        debug2 << "[amrex-plugin] Skipping particle header '"
               << headerPath << "' (no parse error details)\n";
      }
      continue;
    }
    debug2 << "[amrex-plugin] Parsed particle header '" << headerPath
           << "' dim=" << header.spatialDim
           << " real=" << header.realNames.size()
           << " int=" << header.intNames.size()
           << " finest=" << header.finestLevel << "\n";
    if (header.spatialDim != plotfile.spaceDim()) {
      debug1 << "[amrex-plugin] Particle header '" << headerPath
             << "' dim=" << header.spatialDim
             << " differs from plotfile dim=" << plotfile.spaceDim() << "\n";
    }

    ParticleSpeciesInfo info;
    info.meshName = "particles/" + entry;
    info.speciesName = entry;
    info.baseMeshName = baseMeshName;
    info.header = header;
    info.speciesDir = entryPath;
    info.realComponents = header.realNames;
    info.intComponents.clear();
    if (header.isCheckpoint) {
      info.intComponents.push_back("id");
      info.intComponents.push_back("cpu");
    }
    info.intComponents.insert(info.intComponents.end(),
                              header.intNames.begin(),
                              header.intNames.end());
    info.spatialDim = header.spatialDim;
    info.legacyHeader = header.legacy;

    std::vector<int> numGrids = info.header.numGrids;
    if (numGrids.empty() && info.legacyHeader) {
      LegacyParticleHeader legacy;
      std::string legacyError;
      if (ParseLegacyParticleHeader(headerPath, legacy, legacyError)) {
        numGrids = legacy.gridsPerLevel;
      } else {
        debug1 << "[amrex-plugin] Failed to parse legacy particle header '"
               << headerPath << "': " << legacyError << "\n";
      }
    }
    if (numGrids.empty()) {
      debug1 << "[amrex-plugin] Particle header '" << headerPath
             << "' missing grid metadata; skipping hierarchy build\n";
      continue;
    }
    info.header.numGrids = numGrids;
    speciesCache.emplace(entry, info);
    meshMap_[info.meshName] = std::tuple(DatasetType::Particle, entry);

    MeshPatchHierarchy particleHierarchy =
        buildParticleHierarchy(info.meshName, info.spatialDim, numGrids);
    hierarchyMap[info.meshName] = particleHierarchy;

    if (md != nullptr) {
      avtMeshMetaData *meshMd = new avtMeshMetaData;
      meshMd->name = info.meshName;
      meshMd->meshType = AVT_POINT_MESH;
      meshMd->topologicalDimension = 0;
      meshMd->spatialDimension = info.spatialDim;
      meshMd->numBlocks = static_cast<int>(particleHierarchy.patches.size());
      meshMd->blockTitle = "patches";
      meshMd->blockPieceName = "patch";
      meshMd->numGroups = particleHierarchy.numLevels;
      meshMd->groupTitle = "levels";
      meshMd->groupPieceName = "level";
      meshMd->blockNames = particleHierarchy.blockNames;
      meshMd->containsGhostZones = AVT_NO_GHOSTS;
      md->Add(meshMd);
      intVector blockIds = particleHierarchy.groupIds;
      md->AddGroupInformation(particleHierarchy.numLevels,
                              static_cast<int>(particleHierarchy.patches.size()),
                              blockIds);
    }

    std::string positionVar = info.meshName + "/position";
    ParticleVectorVarInfo positionInfo;
    positionInfo.meshName = info.meshName;
    positionInfo.speciesName = entry;
    positionInfo.isReal = true;
    positionInfo.isPosition = true;
    positionInfo.componentIndices.clear();
    for (int axis = 0; axis < info.spatialDim && axis < 3; ++axis) {
      positionInfo.componentIndices.push_back(axis);
    }
    if (!positionInfo.componentIndices.empty()) {
      particleVectorVarMap_[positionVar] = positionInfo;
      if (md != nullptr) {
        AddVectorVarToMetaData(md, positionVar.c_str(),
                               info.meshName.c_str(), AVT_NODECENT,
                               static_cast<int>(positionInfo.componentIndices.size()));
      }
    }

    for (size_t idx = 0; idx < info.realComponents.size(); ++idx) {
      std::string name = info.realComponents[idx];
      if (name.empty()) {
        continue;
      }
      std::string varName = info.meshName + "/" + name;
      ParticleVarInfo varInfo;
      varInfo.meshName = info.meshName;
      varInfo.speciesName = entry;
      const int legacyOffset = info.legacyHeader ? 0 : info.spatialDim;
      varInfo.componentIndex = static_cast<int>(idx) + legacyOffset;
      varInfo.isReal = true;
      particleVarMap_[varName] = varInfo;
      if (md != nullptr) {
        AddScalarVarToMetaData(md, varName.c_str(), info.meshName.c_str(),
                               AVT_NODECENT);
      }
    }

    for (size_t idx = 0; idx < info.intComponents.size(); ++idx) {
      std::string name = info.intComponents[idx];
      if (name.empty()) {
        continue;
      }
      std::string varName = info.meshName + "/" + name;
      ParticleVarInfo varInfo;
      varInfo.meshName = info.meshName;
      varInfo.speciesName = entry;
      varInfo.componentIndex = static_cast<int>(idx);
      varInfo.isReal = false;
      particleVarMap_[varName] = varInfo;
      if (md != nullptr) {
        AddScalarVarToMetaData(md, varName.c_str(), info.meshName.c_str(),
                               AVT_NODECENT);
      }
    }

    debug2 << "[amrex-plugin] Registered particle species '" << entry
           << "' mesh='" << info.meshName
           << "' real=" << info.realComponents.size()
           << " int=" << info.intComponents.size() << "\n";
  }
}

void avtamrexFileFormat::RegisterFieldVariables(
    avtDatabaseMetaData *md, const std::string &meshName,
    amrex::PlotFileData &plotfile) {
  const auto &varNames = plotfile.varNames();
  std::map<std::string, std::map<std::string, std::string>> vectorCandidates;
  const int spatialDim = plotfile.spaceDim();

  for (const auto &var : varNames) {
    varMap_[var] = std::tuple(meshName, var);
    if (md != nullptr) {
      AddScalarVarToMetaData(md, var.c_str(), meshName.c_str(), AVT_ZONECENT);
    }

    auto pos = var.find_last_of('_');
    if (pos == std::string::npos || pos + 1 >= var.size()) {
      continue;
    }

    std::string base = var.substr(0, pos);
    std::string axis = var.substr(pos + 1);
    std::transform(axis.begin(), axis.end(), axis.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (axis == "x" || axis == "y" || axis == "z") {
      vectorCandidates[base][axis] = var;
    }
  }

  const std::vector<std::string> axisOrder = {"x", "y", "z"};
  for (auto &candidate : vectorCandidates) {
    std::vector<std::string> components;
    bool valid = true;
    for (int axis = 0; axis < spatialDim && axis < 3; ++axis) {
      auto it = candidate.second.find(axisOrder[axis]);
      if (it == candidate.second.end()) {
        valid = false;
        break;
      }
      components.push_back(it->second);
    }
    if (!valid || components.empty()) {
      continue;
    }
    vectorVarMap_[candidate.first] = std::tuple(meshName, components);
    if (md != nullptr) {
      AddVectorVarToMetaData(md, candidate.first.c_str(), meshName.c_str(),
                             AVT_ZONECENT, static_cast<int>(components.size()));
    }
  }
}

MeshPatchHierarchy avtamrexFileFormat::BuildHierarchyFromPlotfile(
    const std::string &visitMeshName, amrex::PlotFileData &plotfile) {
  MeshPatchHierarchy hierarchy;
  hierarchy.numLevels = plotfile.finestLevel() + 1;
  hierarchy.topologicalDim = plotfile.spaceDim();
  hierarchy.spatialDim = plotfile.spaceDim();
  hierarchy.levelValues.resize(hierarchy.numLevels);
  hierarchy.patchesPerLevel.assign(hierarchy.numLevels, {});
  hierarchy.levelCellSizes.assign(hierarchy.numLevels,
                                  std::array<double, 3>{0.0, 0.0, 0.0});

  auto probLo = plotfile.probLo();
  for (int level = 0; level < hierarchy.numLevels; ++level) {
    hierarchy.levelValues[level] = level;
    auto dx = plotfile.cellSize(level);
    hierarchy.levelCellSizes[level] =
        std::array<double, 3>{dx[0], dx[1], dx[2]};

    const amrex::BoxArray &boxes = plotfile.boxArray(level);
    hierarchy.patchesPerLevel[level].reserve(boxes.size());
    for (int idx = 0; idx < boxes.size(); ++idx) {
      const amrex::Box &box = boxes[idx];
      PatchInfo patch;
      patch.meshName = visitMeshName;
      patch.level = level;
      patch.fabIndex = idx;
      patch.cellBox = box;
      patch.centering = AVT_ZONECENT;
      patch.spatialDim = hierarchy.spatialDim;
      patch.offset = {box.smallEnd(0), box.smallEnd(1), box.smallEnd(2)};
      patch.extent = {static_cast<uint64_t>(box.length(0)),
                      static_cast<uint64_t>(box.length(1)),
                      static_cast<uint64_t>(box.length(2))};

      for (int axis = 0; axis < 3; ++axis) {
        patch.spacing[axis] = dx[axis];
        patch.origin[axis] =
            probLo[axis] + static_cast<double>(patch.offset[axis]) * dx[axis];
      }

      ComputeLogicalExtents(patch);

      hierarchy.patches.push_back(patch);
      int patchIndex = static_cast<int>(hierarchy.patches.size()) - 1;
      hierarchy.levelIdsPerPatch.push_back(level);
      hierarchy.groupIds.push_back(level);
      hierarchy.patchesPerLevel[level].push_back(patchIndex);

      std::ostringstream blockName;
      blockName << "level" << level << ",patch" << idx;
      hierarchy.blockNames.push_back(blockName.str());
    }
  }

  hierarchy.levelRefinementRatios.clear();
  if (hierarchy.numLevels > 1) {
    for (int level = 0; level < hierarchy.numLevels - 1; ++level) {
      int ratio = plotfile.refRatio(level);
      hierarchy.levelRefinementRatios.push_back(
          std::array<int, 3>{ratio, ratio, ratio});
    }
  }

  return hierarchy;
}


// ****************************************************************************
//  Method: avtamrexFileFormat::PopulateDatabaseMetaData
//
//  Purpose:
//      This database meta-data object is like a table of contents for the
//      file.  By populating it, you are telling the rest of VisIt what
//      information it can request from you.
//
//  Programmer: benwibking -- generated by xml2avt
//  Creation:   Fri Dec 6 17:16:49 PST 2024
//
// ****************************************************************************

void avtamrexFileFormat::PopulateDatabaseMetaData(avtDatabaseMetaData *md,
                                                    int timeState) {
  debug1 << "[amrex-plugin] PopulateDatabaseMetaData timeState=" << timeState
         << "\n";

  if (timeState < 0 ||
      timeState >= static_cast<int>(plotfilePaths_.size())) {
    EXCEPTION1(InvalidVariableException, "timestep out of range");
  }

  varMap_.clear();
  vectorVarMap_.clear();
  meshMap_.clear();
  particleVarMap_.clear();
  particleVectorVarMap_.clear();

  EnsureHierarchyInitialized(timeState);
  // Ensure all ranks build PlotFileDataImpl together to avoid
  // mismatched collective reads when constructing VisMF headers.
  GetPlotFileImpl(timeState);

  auto plotfile = GetPlotFile(timeState);
  BuildFieldHierarchy(md, *plotfile, timeState);
  BuildParticleHierarchy(md, *plotfile, timeState);

  debug1 << "[amrex-plugin] PopulateDatabaseMetaData complete for timestep="
         << timeState << " meshes=" << meshMap_.size()
         << " vars=" << varMap_.size()
         << " particleVars=" << particleVarMap_.size() << "\n";
}

void avtamrexFileFormat::GetCycles(std::vector<int> &cycles) {
  cycles.resize(iterationIndex_.size());
  for (size_t i = 0; i < iterationIndex_.size(); ++i) {
    cycles[i] = static_cast<int>(iterationIndex_[i]);
  }
}

void avtamrexFileFormat::GetTimes(std::vector<double> &times) {
  times.resize(plotfilePaths_.size());
  for (size_t i = 0; i < plotfilePaths_.size(); ++i) {
    if (!std::isfinite(timeValues_[i])) {
      auto plotfile = GetPlotFile(static_cast<int>(i));
      timeValues_[i] = plotfile->time();
    }
    times[i] = timeValues_[i];
  }
}

void avtamrexFileFormat::EnsureHierarchyInitialized(int timeState) {
  if (timeState < 0 ||
      timeState >= static_cast<int>(meshHierarchyCache_.size())) {
    debug1 << "[amrex-plugin] EnsureHierarchyInitialized out-of-range"
           << " timeState=" << timeState
           << " cacheSize=" << meshHierarchyCache_.size() << "\n";
    EXCEPTION1(InvalidVariableException, "timestep out of range");
  }

  auto &hierarchyMap = meshHierarchyCache_.at(timeState);
  if (!hierarchyMap.empty()) {
    return;
  }

  debug1 << "[amrex-plugin] Lazy hierarchy initialization for timeState="
         << timeState << "\n";
  PopulateHierarchyCache(timeState);
}

void *avtamrexFileFormat::GetAuxiliaryData(const char *var, int timestep,
                                             int domain, const char *type,
                                             void *args, DestructorFunction &df) {
  (void)args;

  const char *varName = var != nullptr ? var : "<null>";
  const char *typeName = type != nullptr ? type : "<null>";
  debug1 << "[amrex-plugin] GetAuxiliaryData type=" << typeName
         << " var=" << varName << " timestep=" << timestep
         << " domain=" << domain << "\n";

  auto resolveMesh = [&](std::string &meshNameOut,
                         const MeshPatchHierarchy *&hierarchyOut) -> bool {
    if (var == nullptr) {
      debug1 << "[amrex-plugin] Auxiliary request missing var name\n";
      return false;
    }

    if (timestep < 0 ||
        timestep >= static_cast<int>(meshHierarchyCache_.size())) {
      debug1 << "[amrex-plugin] Auxiliary request out-of-range timestep\n";
      return false;
    }

    EnsureHierarchyInitialized(timestep);

    std::string requestedName(var);
    std::string meshName = requestedName;
    auto &hierarchyMap = meshHierarchyCache_[timestep];
    auto it = hierarchyMap.find(meshName);
    if (it == hierarchyMap.end()) {
      auto varIt = varMap_.find(requestedName);
      if (varIt != varMap_.end()) {
        meshName = std::get<0>(varIt->second);
        it = hierarchyMap.find(meshName);
        debug1 << "[amrex-plugin] Auxiliary request for var '"
               << requestedName << "' mapped to mesh '" << meshName << "'\n";
      } else {
        debug1 << "[amrex-plugin] Auxiliary request var '" << requestedName
               << "' not found in varMap_ (size=" << varMap_.size() << ")\n";
      }
    }
    if (it == hierarchyMap.end()) {
      auto vecIt = vectorVarMap_.find(requestedName);
      if (vecIt != vectorVarMap_.end()) {
        meshName = std::get<0>(vecIt->second);
        it = hierarchyMap.find(meshName);
        debug1 << "[amrex-plugin] Auxiliary request for vector var '"
               << requestedName << "' mapped to mesh '" << meshName << "'\n";
      } else {
        debug1 << "[amrex-plugin] Auxiliary request var '" << requestedName
               << "' not found in vectorVarMap_ (size="
               << vectorVarMap_.size() << ")\n";
      }
    }
    if (it == hierarchyMap.end()) {
      debug1 << "[amrex-plugin] Auxiliary request missing mesh '"
             << meshName << "'\n";
      return false;
    }

    auto meshTypeIt = meshMap_.find(meshName);
    if (meshTypeIt != meshMap_.end() &&
        std::get<0>(meshTypeIt->second) == DatasetType::Particle) {
      debug2 << "[amrex-plugin] Auxiliary request skipping particle mesh '"
             << meshName << "'\n";
      return false;
    }

    const MeshPatchHierarchy &hierarchy = it->second;
    if (hierarchy.patches.empty()) {
      debug1 << "[amrex-plugin] Auxiliary request mesh '" << meshName
             << "' has no patches\n";
      return false;
    }

    meshNameOut = meshName;
    hierarchyOut = &hierarchy;
    return true;
  };

  if (type != nullptr &&
      strcmp(type, AUXILIARY_DATA_DOMAIN_BOUNDARY_INFORMATION) == 0) {
    std::string meshNameResolved;
    const MeshPatchHierarchy *hier = nullptr;
    if (!resolveMesh(meshNameResolved, hier)) {
      return NULL;
    }
    return avtMTMDFileFormat::GetAuxiliaryData(meshNameResolved.c_str(),
                                               timestep, domain, type, args,
                                               df);
  }

  if (type != nullptr) {
    if (strcmp(type, AUXILIARY_DATA_DOMAIN_BOUNDARY_INFORMATION) == 0) {
      debug2 << "[amrex-plugin] Delegating domain boundary request to base cache" << "\n";
      return avtMTMDFileFormat::GetAuxiliaryData(var, timestep, domain, type, args, df);
    }
    const MeshPatchHierarchy *hierarchyPtr = nullptr;
    std::string meshName;
    auto requireHierarchy = [&]() -> bool {
      if (hierarchyPtr != nullptr) {
        return true;
      }
      return resolveMesh(meshName, hierarchyPtr);
    };

    if (strcmp(type, AUXILIARY_DATA_DOMAIN_NESTING_INFORMATION) == 0) {
      if (!requireHierarchy()) {
        return NULL;
      }
      const MeshPatchHierarchy &hierarchy = *hierarchyPtr;
      debug2 << "[amrex-plugin] Building domain nesting for mesh '"
             << meshName << "'\n";
      avtStructuredDomainNesting *nesting = BuildDomainNesting(hierarchy);
      if (nesting == nullptr) {
        debug1 << "[amrex-plugin] Domain nesting build returned nullptr\n";
        return NULL;
      }
      df = DeleteStructuredDomainNesting;
      debug1 << "[amrex-plugin] Domain nesting ready for mesh '"
             << meshName << "'\n";
      return nesting;
    }

    if (strcmp(type, AUXILIARY_DATA_GLOBAL_NODE_IDS) == 0 ||
        strcmp(type, "GLOBAL_NODE_IDS") == 0) {
      if (!requireHierarchy()) {
        return NULL;
      }
      const MeshPatchHierarchy &hierarchy = *hierarchyPtr;
      if (domain < 0 ||
          domain >= static_cast<int>(hierarchy.patches.size())) {
        debug1 << "[amrex-plugin] Global node id request invalid domain "
               << domain << " for mesh '" << meshName << "'\n";
        return NULL;
      }
      vtkIdTypeArray *nodeIds = BuildGlobalNodeIds(hierarchy, domain);
      if (nodeIds == nullptr) {
        debug1 << "[amrex-plugin] Failed to build global node ids for mesh '"
               << meshName << "' domain=" << domain << "\n";
        return NULL;
      }
      df = avtVariableCache::DestructVTKObject;
      debug1 << "[amrex-plugin] Global node ids ready for mesh '"
             << meshName << "' domain=" << domain << "\n";
      return nodeIds;
    }

    if (strcmp(type, AUXILIARY_DATA_GLOBAL_ZONE_IDS) == 0 ||
        strcmp(type, "GLOBAL_ZONE_IDS") == 0) {
      if (!requireHierarchy()) {
        return NULL;
      }
      const MeshPatchHierarchy &hierarchy = *hierarchyPtr;
      if (domain < 0 ||
          domain >= static_cast<int>(hierarchy.patches.size())) {
        debug1 << "[amrex-plugin] Global zone id request invalid domain "
               << domain << " for mesh '" << meshName << "'\n";
        return NULL;
      }
      vtkIdTypeArray *zoneIds = BuildGlobalZoneIds(hierarchy, domain);
      if (zoneIds == nullptr) {
        debug1 << "[amrex-plugin] Failed to build global zone ids for mesh '"
               << meshName << "' domain=" << domain << "\n";
        return NULL;
      }
      df = avtVariableCache::DestructVTKObject;
      debug1 << "[amrex-plugin] Global zone ids ready for mesh '"
             << meshName << "' domain=" << domain << "\n";
      return zoneIds;
    }
  }

  debug2 << "[amrex-plugin] Forwarding auxiliary request to base class\n";
  return avtMTMDFileFormat::GetAuxiliaryData(var, timestep, domain, type, args,
                                             df);
}

std::pair<std::string, int>
avtamrexFileFormat::ParseMeshLevel(std::string const &meshName) const {
  int level = 0;
  std::string base = meshName;

  const std::string suffix = "_lvl";
  std::size_t pos = meshName.rfind(suffix);
  if (pos != std::string::npos) {
    std::size_t digitsBegin = pos + suffix.size();
    if (digitsBegin < meshName.size()) {
      std::size_t digitsEnd = digitsBegin;
      while (digitsEnd < meshName.size() && std::isdigit(meshName[digitsEnd])) {
        ++digitsEnd;
      }
      if (digitsEnd > digitsBegin) {
        level = std::stoi(meshName.substr(digitsBegin, digitsEnd - digitsBegin));
        base = meshName.substr(0, pos);
      }
    }
  }

  return {base, level};
}


vtkDataSet *
avtamrexFileFormat::CreateRectilinearPatch(const PatchInfo &patch) const {
  vtkFloatArray *coords[3];
  int dimensions[3];

  for (int axis = 0; axis < 3; ++axis) {
    const uint64_t cells =
        axis < static_cast<int>(patch.extent.size()) ? patch.extent[axis] : 1;
    int nodes = static_cast<int>(cells);
    if (patch.centering == AVT_ZONECENT) {
      nodes = static_cast<int>(cells + 1);
    }
    if (nodes <= 0) {
      nodes = 1;
    }
    dimensions[axis] = nodes;

    coords[axis] = vtkFloatArray::New();
    coords[axis]->SetNumberOfTuples(nodes);
    float *array = static_cast<float *>(coords[axis]->GetVoidPointer(0));
    for (int idx = 0; idx < nodes; ++idx) {
      array[idx] = static_cast<float>(patch.origin[axis] +
                                      static_cast<double>(idx) *
                                          patch.spacing[axis]);
    }
  }

  vtkRectilinearGrid *grid = vtkRectilinearGrid::New();
  grid->SetDimensions(dimensions);
  grid->SetXCoordinates(coords[0]);
  grid->SetYCoordinates(coords[1]);
  grid->SetZCoordinates(coords[2]);
  debug5 << "[amrex-plugin] CreateRectilinearPatch mesh=" << patch.meshName
         << " level=" << patch.level << " logicalLower=[" << patch.logicalLower[0]
         << "," << patch.logicalLower[1] << "," << patch.logicalLower[2]
         << "] logicalUpper=[" << patch.logicalUpper[0] << ","
         << patch.logicalUpper[1] << "," << patch.logicalUpper[2]
         << "] vtkDims=[" << dimensions[0] << "," << dimensions[1] << ","
         << dimensions[2] << "] centering="
         << (patch.centering == AVT_ZONECENT ? "zone"
                                             : (patch.centering == AVT_NODECENT
                                                    ? "node"
                                                    : "unknown"))
         << "\n";

  for (int axis = 0; axis < 3; ++axis) {
    coords[axis]->Delete();
  }

  return grid;
}

avtStructuredDomainNesting *
avtamrexFileFormat::BuildDomainNesting(
    const MeshPatchHierarchy &hierarchy) const {
  if (hierarchy.patches.empty() || hierarchy.numLevels == 0) {
    debug1 << "[amrex-plugin] BuildDomainNesting received empty hierarchy\n";
    return nullptr;
  }

  debug1 << "[amrex-plugin] BuildDomainNesting patches="
         << hierarchy.patches.size() << " levels=" << hierarchy.numLevels
         << "\n";

  auto *nesting = new avtStructuredDomainNesting(
      static_cast<int>(hierarchy.patches.size()), hierarchy.numLevels);

  const int dims = std::max(1, hierarchy.spatialDim);
  nesting->SetNumDimensions(dims);

  nesting->SetLevelRefinementRatios(0, MakeRefinementVector({1, 1, 1}, dims));
  for (int level = 1; level < hierarchy.numLevels; ++level) {
    std::array<int, 3> ratio{1, 1, 1};
    if (level - 1 < static_cast<int>(hierarchy.levelRefinementRatios.size())) {
      ratio = hierarchy.levelRefinementRatios[level - 1];
    }
    nesting->SetLevelRefinementRatios(level, MakeRefinementVector(ratio, dims));
  }

  for (int level = 0; level < hierarchy.numLevels; ++level) {
    std::array<double, 3> sizes{0.0, 0.0, 0.0};
    if (level < static_cast<int>(hierarchy.levelCellSizes.size())) {
      sizes = hierarchy.levelCellSizes[level];
    }
    nesting->SetLevelCellSizes(level, MakeCellSizeVector(sizes, dims));
  }

  for (size_t patchIdx = 0; patchIdx < hierarchy.patches.size(); ++patchIdx) {
    const PatchInfo &patch = hierarchy.patches[patchIdx];
    const int levelIndex = hierarchy.levelIdsPerPatch[patchIdx];

    if (patchIdx == 0) {
      LogPatchSummary(patch, "BuildDomainNesting reference patch");
    }

    std::vector<int> children;
    if (levelIndex + 1 < hierarchy.numLevels) {
      if (levelIndex >=
          static_cast<int>(hierarchy.levelRefinementRatios.size())) {
        debug1 << "[amrex-plugin] Missing refinement ratio for level "
               << levelIndex << " while building nesting\n";
      } else {
        const auto refRatio = MakeRefRatioIntVect(
            hierarchy.levelRefinementRatios[levelIndex]);
        const auto &candidateChildren =
            hierarchy.patchesPerLevel[levelIndex + 1];
        for (int childIdx : candidateChildren) {
          if (IsChildPatch(patch, hierarchy.patches[childIdx], refRatio)) {
            children.push_back(childIdx);
          }
        }
      }
    }

    std::vector<int> logicalExtents(6, 0);
    for (int axis = 0; axis < 3; ++axis) {
      logicalExtents[axis] = patch.logicalLower[axis];
      logicalExtents[axis + 3] = patch.logicalUpper[axis];
    }

    nesting->SetNestingForDomain(static_cast<int>(patchIdx), levelIndex,
                                 children, logicalExtents);

    debug5 << "[amrex-plugin] Nesting domain=" << patchIdx
           << " level=" << levelIndex << " children="
           << JoinContainer(children) << " logicalExtents="
           << JoinContainer(logicalExtents) << '\n';
  }

  debug1 << "[amrex-plugin] BuildDomainNesting completed\n";
  return nesting;
}

avtLocalStructuredDomainBoundaryList *
avtamrexFileFormat::BuildDomainBoundaryList(
    const MeshPatchHierarchy &hierarchy, int domain) const {
  if (hierarchy.patches.empty()) {
    return nullptr;
  }

  if (domain < 0 || domain >= static_cast<int>(hierarchy.patches.size())) {
    return nullptr;
  }

  const PatchInfo &patch = hierarchy.patches[domain];
  LogPatchSummary(patch, "BuildDomainBoundaryList patch");

  int extents[6] = {0, 0, 0, 0, 0, 0};
  for (int axis = 0; axis < 3; ++axis) {
    extents[2 * axis] = patch.logicalLower[axis];
    extents[2 * axis + 1] = patch.logicalUpper[axis];
  }
  for (int axis = 0; axis < hierarchy.topologicalDim; ++axis) {
    if (patch.centering == AVT_ZONECENT) {
      extents[2 * axis + 1] += 1;
    }
  }
  for (int axis = hierarchy.topologicalDim; axis < 3; ++axis) {
    extents[2 * axis] = 0;
    extents[2 * axis + 1] = 1;
  }

  auto *list = new avtLocalStructuredDomainBoundaryList(domain, extents);

  auto rangesOverlap = [](int a0, int a1, int b0, int b1) {
    return std::max(a0, b0) <= std::min(a1, b1);
  };

  for (size_t otherIdx = 0; otherIdx < hierarchy.patches.size(); ++otherIdx) {
    if (static_cast<int>(otherIdx) == domain) {
      continue;
    }

    const PatchInfo &other = hierarchy.patches[otherIdx];

    int touchAxis = -1;
    int orientation[3] = {0, 0, 0};

    for (int axis = 0; axis < 3; ++axis) {
      bool overlapsOtherDims = true;
      for (int otherAxis = 0; otherAxis < 3; ++otherAxis) {
        if (otherAxis == axis) {
          continue;
        }
        if (!rangesOverlap(patch.logicalLower[otherAxis], patch.logicalUpper[otherAxis],
                           other.logicalLower[otherAxis], other.logicalUpper[otherAxis])) {
          overlapsOtherDims = false;
          break;
        }
      }

      if (!overlapsOtherDims) {
        continue;
      }

      if (patch.logicalUpper[axis] + 1 == other.logicalLower[axis]) {
        if (touchAxis != -1) {
          touchAxis = -2;
          break;
        }
        touchAxis = axis;
        orientation[axis] = 1;
      } else if (other.logicalUpper[axis] + 1 == patch.logicalLower[axis]) {
        if (touchAxis != -1) {
          touchAxis = -2;
          break;
        }
        touchAxis = axis;
        orientation[axis] = -1;
      }
    }

    if (touchAxis == -1) {
      continue;
    }
    if (touchAxis == -2) {
      continue;
    }

    int boundaryExtents[6] = {0, 0, 0, 0, 0, 0};
    for (int axis = 0; axis < 3; ++axis) {
      if (axis == touchAxis) {
        if (orientation[axis] > 0) {
          boundaryExtents[2 * axis] = patch.logicalUpper[axis];
          boundaryExtents[2 * axis + 1] = patch.logicalUpper[axis];
        } else {
          boundaryExtents[2 * axis] = patch.logicalLower[axis];
          boundaryExtents[2 * axis + 1] = patch.logicalLower[axis];
        }
      } else {
        int lower = std::max(patch.logicalLower[axis], other.logicalLower[axis]);
        int upper = std::min(patch.logicalUpper[axis], other.logicalUpper[axis]);
        boundaryExtents[2 * axis] = lower;
        boundaryExtents[2 * axis + 1] = upper;
      }
    }

    for (int axis = 0; axis < hierarchy.topologicalDim; ++axis) {
      if (patch.centering == AVT_ZONECENT) {
        boundaryExtents[2 * axis + 1] += 1;
      }
    }
    for (int axis = hierarchy.topologicalDim; axis < 3; ++axis) {
      boundaryExtents[2 * axis] = 0;
      boundaryExtents[2 * axis + 1] = 1;
    }

    debug5 << "[amrex-plugin] LocalBoundary domain=" << domain
           << " neighbor=" << otherIdx << " extents=["
           << boundaryExtents[0] << "," << boundaryExtents[1] << " ; "
           << boundaryExtents[2] << "," << boundaryExtents[3] << " ; "
           << boundaryExtents[4] << "," << boundaryExtents[5] << "]\n";

    list->AddNeighbor(static_cast<int>(otherIdx), static_cast<int>(domain), orientation, boundaryExtents);
  }

  return list;

}

avtStructuredDomainBoundaries *
avtamrexFileFormat::BuildStructuredDomainBoundaries(
    const MeshPatchHierarchy &hierarchy) const {
  if (hierarchy.patches.empty()) {
    return nullptr;
  }

  auto *boundaries = new avtRectilinearDomainBoundaries(true);
  boundaries->SetNumDomains(static_cast<int>(hierarchy.patches.size()));

  for (size_t patchIdx = 0; patchIdx < hierarchy.patches.size(); ++patchIdx) {
    const PatchInfo &patch = hierarchy.patches[patchIdx];
    int extents[6] = {0, 0, 0, 0, 0, 0};
    for (int axis = 0; axis < 3; ++axis) {
      if (axis >= hierarchy.topologicalDim) {
        extents[2 * axis] = 0;
        extents[2 * axis + 1] = 1;
        continue;
      }
      extents[2 * axis] = patch.logicalLower[axis];
      int upperExclusive = patch.logicalUpper[axis] + 1;
      extents[2 * axis + 1] = upperExclusive;
    }
    debug5 << "[amrex-plugin] StructuredBoundaries patch=" << patchIdx
           << " level=" << (patchIdx < hierarchy.levelIdsPerPatch.size()
                                ? hierarchy.levelIdsPerPatch[patchIdx]
                                : 0)
           << " extents=[" << extents[0] << "," << extents[1] << " ; "
           << extents[2] << "," << extents[3] << " ; " << extents[4] << ","
           << extents[5] << "]\n";
    int level = 0;
    if (patchIdx < hierarchy.levelIdsPerPatch.size()) {
      level = hierarchy.levelIdsPerPatch[patchIdx];
    }
    boundaries->SetIndicesForAMRPatch(static_cast<int>(patchIdx), level,
                                      extents);
  }

  boundaries->CalculateBoundaries();
  return boundaries;
}

vtkIdTypeArray *
avtamrexFileFormat::BuildGlobalZoneIds(const MeshPatchHierarchy &hierarchy,
                                         int domain) const {
  if (hierarchy.patches.empty() || domain < 0 ||
      domain >= static_cast<int>(hierarchy.patches.size())) {
    return nullptr;
  }

  bool meshNodeCentered = MeshIsNodeCentered(hierarchy);
  std::array<int, 3> globalDims =
      ComputeGlobalCellDimensions(hierarchy, meshNodeCentered);
  const PatchInfo &patch = hierarchy.patches[domain];
  std::array<int, 3> counts =
      ComputePatchCellCounts(patch, hierarchy.topologicalDim, meshNodeCentered);
  debug5 << "[amrex-plugin] GlobalZoneIds domain=" << domain
         << " globalDims=[" << globalDims[0] << "," << globalDims[1] << ","
         << globalDims[2] << "]"
         << " logicalLower=[" << patch.logicalLower[0] << ","
         << patch.logicalLower[1] << "," << patch.logicalLower[2]
         << "] counts=[" << counts[0] << "," << counts[1] << "," << counts[2]
         << "]\n";

  vtkIdType totalTuples = static_cast<vtkIdType>(counts[0]) *
                          static_cast<vtkIdType>(counts[1]) *
                          static_cast<vtkIdType>(counts[2]);
  vtkIdTypeArray *ids = vtkIdTypeArray::New();
  ids->SetName("avtGlobalZoneId");
  ids->SetNumberOfComponents(1);
  ids->SetNumberOfTuples(totalTuples);

  vtkIdType strideY = static_cast<vtkIdType>(globalDims[0]);
  vtkIdType strideZ = strideY * static_cast<vtkIdType>(globalDims[1]);
  vtkIdType *values = static_cast<vtkIdType *>(ids->GetVoidPointer(0));
  vtkIdType idx = 0;

  for (int k = 0; k < counts[2]; ++k) {
    int gk = patch.logicalLower[2] + k;
    for (int j = 0; j < counts[1]; ++j) {
      int gj = patch.logicalLower[1] + j;
      for (int i = 0; i < counts[0]; ++i) {
        int gi = patch.logicalLower[0] + i;
        vtkIdType globalId = static_cast<vtkIdType>(gi) +
                             strideY * static_cast<vtkIdType>(gj) +
                             strideZ * static_cast<vtkIdType>(gk);
        values[idx++] = globalId;
      }
    }
  }

  return ids;
}

vtkIdTypeArray *
avtamrexFileFormat::BuildGlobalNodeIds(const MeshPatchHierarchy &hierarchy,
                                         int domain) const {
  if (hierarchy.patches.empty() || domain < 0 ||
      domain >= static_cast<int>(hierarchy.patches.size())) {
    return nullptr;
  }

  bool meshNodeCentered = MeshIsNodeCentered(hierarchy);
  std::array<int, 3> globalDims =
      ComputeGlobalNodeDimensions(hierarchy, meshNodeCentered);
  const PatchInfo &patch = hierarchy.patches[domain];
  std::array<int, 3> counts =
      ComputePatchNodeCounts(patch, hierarchy.topologicalDim, meshNodeCentered);
  debug5 << "[amrex-plugin] GlobalNodeIds domain=" << domain
         << " globalDims=[" << globalDims[0] << "," << globalDims[1] << ","
         << globalDims[2] << "]"
         << " logicalLower=[" << patch.logicalLower[0] << ","
         << patch.logicalLower[1] << "," << patch.logicalLower[2]
         << "] counts=[" << counts[0] << "," << counts[1] << "," << counts[2]
         << "]\n";

  vtkIdType totalTuples = static_cast<vtkIdType>(counts[0]) *
                          static_cast<vtkIdType>(counts[1]) *
                          static_cast<vtkIdType>(counts[2]);
  vtkIdTypeArray *ids = vtkIdTypeArray::New();
  ids->SetName("avtGlobalNodeId");
  ids->SetNumberOfComponents(1);
  ids->SetNumberOfTuples(totalTuples);

  vtkIdType strideY = static_cast<vtkIdType>(globalDims[0]);
  vtkIdType strideZ = strideY * static_cast<vtkIdType>(globalDims[1]);
  vtkIdType *values = static_cast<vtkIdType *>(ids->GetVoidPointer(0));
  vtkIdType idx = 0;

  for (int k = 0; k < counts[2]; ++k) {
    int gk = patch.logicalLower[2] + k;
    for (int j = 0; j < counts[1]; ++j) {
      int gj = patch.logicalLower[1] + j;
      for (int i = 0; i < counts[0]; ++i) {
        int gi = patch.logicalLower[0] + i;
        vtkIdType globalId = static_cast<vtkIdType>(gi) +
                             strideY * static_cast<vtkIdType>(gj) +
                             strideZ * static_cast<vtkIdType>(gk);
        values[idx++] = globalId;
      }
    }
  }

  return ids;
}

void avtamrexFileFormat::AddGhostZonesForPatch(
    const MeshPatchHierarchy &hierarchy, int patchIdx,
    vtkRectilinearGrid *grid) const {
  if (grid == nullptr) {
    return;
  }

  if (patchIdx < 0 || patchIdx >= static_cast<int>(hierarchy.patches.size())) {
    return;
  }

  const PatchInfo &patch = hierarchy.patches[patchIdx];
  if (patch.centering != AVT_ZONECENT) {
    return;
  }

  const int levelIndex = hierarchy.levelIdsPerPatch[patchIdx];
  if (levelIndex + 1 >= hierarchy.numLevels) {
    return;
  }

  const auto &candidateChildren = hierarchy.patchesPerLevel[levelIndex + 1];
  if (candidateChildren.empty()) {
    return;
  }

  if (levelIndex >= static_cast<int>(hierarchy.levelRefinementRatios.size())) {
    debug1 << "[amrex-plugin] Missing refinement ratio for level "
           << levelIndex << " while building ghost zones\n";
    return;
  }
  const auto refRatio =
      MakeRefRatioIntVect(hierarchy.levelRefinementRatios[levelIndex]);

  const int nx = patch.extent.size() > 0
                     ? static_cast<int>(patch.extent[0])
                     : 1;
  const int ny = patch.extent.size() > 1
                     ? static_cast<int>(patch.extent[1])
                     : 1;
  const int nz = patch.extent.size() > 2
                     ? static_cast<int>(patch.extent[2])
                     : 1;

  const vtkIdType cellCount = static_cast<vtkIdType>(nx) * ny * nz;
  if (cellCount <= 0) {
    return;
  }

  vtkUnsignedCharArray *ghostArray = vtkUnsignedCharArray::New();
  ghostArray->SetNumberOfComponents(1);
  ghostArray->SetNumberOfTuples(cellCount);
  ghostArray->SetName("avtGhostZones");

  unsigned char *values = ghostArray->GetPointer(0);
  std::fill(values, values + cellCount, 0);

  bool markedAny = false;
  for (int childIdx : candidateChildren) {
    if (childIdx < 0 ||
        childIdx >= static_cast<int>(hierarchy.patches.size())) {
      continue;
    }

    const PatchInfo &child = hierarchy.patches[childIdx];
    if (!IsChildPatch(patch, child, refRatio)) {
      continue;
    }

    amrex::Box fineOnCoarse = amrex::coarsen(child.cellBox, refRatio);
    amrex::Box overlap = fineOnCoarse & patch.cellBox;
    if (!overlap.ok()) {
      continue;
    }

    const amrex::IntVect localLo =
        overlap.smallEnd() - patch.cellBox.smallEnd();
    const amrex::IntVect localHi =
        overlap.bigEnd() - patch.cellBox.smallEnd();

    const int x0 = std::max(0, localLo[0]);
    const int x1 = std::min(nx - 1, localHi[0]);
    const int y0 = std::max(0, localLo[1]);
    const int y1 = std::min(ny - 1, localHi[1]);
    const int z0 = std::max(0, localLo[2]);
    const int z1 = std::min(nz - 1, localHi[2]);

    if (x0 > x1 || y0 > y1 || z0 > z1) {
      continue;
    }

    for (int z = z0; z <= z1; ++z) {
      for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
          const vtkIdType cellId =
              x + static_cast<vtkIdType>(nx) *
                      (y + static_cast<vtkIdType>(ny) * z);
          avtGhostData::AddGhostZoneType(values[cellId],
                                         REFINED_ZONE_IN_AMR_GRID);
        }
      }
    }
    markedAny = true;
  }

  if (markedAny) {
    grid->GetCellData()->AddArray(ghostArray);
  }
  ghostArray->Delete();
}

vtkDataArray *avtamrexFileFormat::LoadScalarPatchData(
    int timeState, const PatchInfo &patch, const std::string &component) const {
  debug1 << "[amrex-plugin] LoadScalarPatchData mesh='" << patch.meshName
         << "' component='" << component << "' offset="
         << JoinContainer(patch.offset) << " extent="
         << JoinContainer(patch.extent) << "\n";

  auto plotfile = GetPlotFile(timeState);
  const auto &varNames = plotfile->varNames();
  auto varIt = std::find(varNames.begin(), varNames.end(), component);
  if (varIt == varNames.end()) {
    debug1 << "[amrex-plugin] LoadScalarPatchData missing component '"
           << component << "'\n";
    EXCEPTION1(InvalidVariableException, component.c_str());
  }
  int compIndex = static_cast<int>(varIt - varNames.begin());

  debug1 << "[amrex-plugin] LoadScalarPatchData VisMF read level="
         << patch.level << " component='" << component << "'\n";
  auto vismf = GetVisMF(timeState, patch.level);
  if (vismf == nullptr) {
    debug1 << "[amrex-plugin] LoadScalarPatchData VisMF missing for level "
           << patch.level << "\n";
    EXCEPTION1(InvalidFilesException, component.c_str());
  }
  const int fabCount = vismf->size();
  if (patch.fabIndex < 0 || patch.fabIndex >= fabCount) {
    debug1 << "[amrex-plugin] LoadScalarPatchData invalid fab index "
           << patch.fabIndex << " for level " << patch.level
           << " with fab count " << fabCount << "\n";
    EXCEPTION1(InvalidFilesException, component.c_str());
  }
  const int mfComponents = vismf->nComp();
  if (compIndex < 0 || compIndex >= mfComponents) {
    debug1 << "[amrex-plugin] LoadScalarPatchData component index "
           << compIndex << " out of range for MultiFab nComp="
           << mfComponents << "\n";
    EXCEPTION1(InvalidVariableException, component.c_str());
  }

  const amrex::FArrayBox &fab = vismf->GetFab(patch.fabIndex, compIndex);
  QueueVisMFClear({timeState, patch.level}, patch.fabIndex, compIndex);
  int fabComp = compIndex;
  if (fab.nComp() == 1 && compIndex != 0) {
    // VisMF::GetFab(fab, comp) returns a single-component FArrayBox.
    fabComp = 0;
  } else if (compIndex < 0 || compIndex >= fab.nComp()) {
    debug1 << "[amrex-plugin] LoadScalarPatchData component index "
           << compIndex << " out of range for fab nComp=" << fab.nComp()
           << "\n";
    EXCEPTION1(InvalidVariableException, component.c_str());
  }
  const amrex::Box &box = fab.box();

  int nx = static_cast<int>(patch.extent.size() > 0 ? patch.extent[0] : 1);
  int ny = static_cast<int>(patch.extent.size() > 1 ? patch.extent[1] : 1);
  int nz = static_cast<int>(patch.extent.size() > 2 ? patch.extent[2] : 1);
  if (patch.spatialDim < 2) {
    ny = 1;
  }
  if (patch.spatialDim < 3) {
    nz = 1;
  }

  vtkIdType tupleCount =
      static_cast<vtkIdType>(nx) * static_cast<vtkIdType>(ny) * static_cast<vtkIdType>(nz);
  vtkDoubleArray *array = vtkDoubleArray::New();
  array->SetNumberOfComponents(1);
  array->SetNumberOfTuples(tupleCount);
  double *buffer = array->GetPointer(0);

  const int loX = box.smallEnd(0);
  const int loY = box.smallEnd(1);
  const int loZ = box.smallEnd(2);
  const int fabNx = box.length(0);
  const int fabNy = box.length(1);
  const int fabNz = box.length(2);
  const amrex::Real *src = fab.dataPtr(fabComp);

  if (nx == fabNx && ny == fabNy && nz == fabNz) {
    std::copy(src, src + static_cast<vtkIdType>(tupleCount), buffer);
  } else {
    vtkIdType idx = 0;
    for (int k = 0; k < nz; ++k) {
      int kk = loZ + (patch.spatialDim >= 3 ? k : 0);
      const int kOffset = kk - loZ;
      for (int j = 0; j < ny; ++j) {
        int jj = loY + (patch.spatialDim >= 2 ? j : 0);
        const int jOffset = jj - loY;
        const amrex::Real *row =
            src + (kOffset * fabNy + jOffset) * fabNx;
        std::copy(row, row + nx, buffer + idx);
        idx += nx;
      }
    }
  }

  DuplicateHighEndNodes(patch, buffer);
  return array;
}

vtkDataArray *avtamrexFileFormat::LoadVectorPatchData(
    int timeState, const PatchInfo &patch,
    const std::vector<std::string> &components) const {
  debug1 << "[amrex-plugin] LoadVectorPatchData mesh='" << patch.meshName
         << "' components=" << JoinStrings(components) << "' offset="
         << JoinContainer(patch.offset) << " extent="
         << JoinContainer(patch.extent) << "\n";

  if (components.empty()) {
    debug1 << "[amrex-plugin] LoadVectorPatchData invoked with no"
           << " components; returning nullptr\n";
    return nullptr;
  }

  vtkDataArray *result = nullptr;
  std::vector<vtkDataArray *> loadedComponents;
  loadedComponents.reserve(components.size());

  try {
    for (auto const &componentName : components) {
      vtkDataArray *componentData =
          LoadScalarPatchData(timeState, patch, componentName);
      if (componentData == nullptr) {
        debug1 << "[amrex-plugin] LoadVectorPatchData failed to load"
               << " component '" << componentName << "'\n";
        EXCEPTION1(InvalidVariableException, componentName.c_str());
      }
      loadedComponents.push_back(componentData);
    }

    vtkIdType tupleCount = loadedComponents.front()->GetNumberOfTuples();
    bool hasDoubleComponent = false;
    bool hasFloatComponent = false;

    for (vtkDataArray *component : loadedComponents) {
      if (component->GetNumberOfComponents() != 1) {
        debug1 << "[amrex-plugin] LoadVectorPatchData component has"
               << " unexpected component count="
               << component->GetNumberOfComponents() << "\n";
        EXCEPTION1(InvalidVariableException, patch.meshName.c_str());
      }
      if (component->GetNumberOfTuples() != tupleCount) {
        debug1 << "[amrex-plugin] LoadVectorPatchData tuple count"
               << " mismatch for a component\n";
        EXCEPTION1(InvalidVariableException, patch.meshName.c_str());
      }
      if (vtkDoubleArray::SafeDownCast(component) != nullptr) {
        hasDoubleComponent = true;
      } else if (vtkFloatArray::SafeDownCast(component) != nullptr) {
        hasFloatComponent = true;
      } else {
        debug1 << "[amrex-plugin] LoadVectorPatchData unsupported"
               << " vtkDataArray subtype for component\n";
        EXCEPTION1(InvalidVariableException, patch.meshName.c_str());
      }
    }

    const int numComponents = static_cast<int>(components.size());
    if (hasDoubleComponent) {
      vtkDoubleArray *vectorArray = vtkDoubleArray::New();
      vectorArray->SetNumberOfComponents(numComponents);
      vectorArray->SetNumberOfTuples(tupleCount);
      double *dest = vectorArray->GetPointer(0);

      for (size_t compIdx = 0; compIdx < loadedComponents.size(); ++compIdx) {
        vtkDataArray *component = loadedComponents[compIdx];
        if (auto *doubleComp = vtkDoubleArray::SafeDownCast(component)) {
          const double *src = doubleComp->GetPointer(0);
          for (vtkIdType tuple = 0; tuple < tupleCount; ++tuple) {
            dest[tuple * numComponents + static_cast<int>(compIdx)] = src[tuple];
          }
        } else if (auto *floatComp = vtkFloatArray::SafeDownCast(component)) {
          const float *src = floatComp->GetPointer(0);
          for (vtkIdType tuple = 0; tuple < tupleCount; ++tuple) {
            dest[tuple * numComponents + static_cast<int>(compIdx)] =
                static_cast<double>(src[tuple]);
          }
        }
      }

      result = vectorArray;
    } else if (hasFloatComponent) {
      vtkFloatArray *vectorArray = vtkFloatArray::New();
      vectorArray->SetNumberOfComponents(numComponents);
      vectorArray->SetNumberOfTuples(tupleCount);
      float *dest = vectorArray->GetPointer(0);

      for (size_t compIdx = 0; compIdx < loadedComponents.size(); ++compIdx) {
        auto *floatComp = vtkFloatArray::SafeDownCast(loadedComponents[compIdx]);
        if (floatComp == nullptr) {
        debug1 << "[amrex-plugin] LoadVectorPatchData expected"
               << " float component but found different type\n";
        EXCEPTION1(InvalidVariableException, patch.meshName.c_str());
      }
        const float *src = floatComp->GetPointer(0);
        for (vtkIdType tuple = 0; tuple < tupleCount; ++tuple) {
          dest[tuple * numComponents + static_cast<int>(compIdx)] = src[tuple];
        }
      }

      result = vectorArray;
    }

    if (result == nullptr) {
      debug1 << "[amrex-plugin] LoadVectorPatchData could not determine"
             << " a suitable output array type\n";
      EXCEPTION1(InvalidVariableException, patch.meshName.c_str());
    }

    for (vtkDataArray *component : loadedComponents) {
      component->Delete();
    }
    loadedComponents.clear();

    debug1 << "[amrex-plugin] LoadVectorPatchData returning array"
           << " tuples=" << tupleCount << " components=" << numComponents
           << "\n";

    return result;
  } catch (...) {
    for (vtkDataArray *component : loadedComponents) {
      component->Delete();
    }
    if (result != nullptr) {
      result->Delete();
    }
    throw;
  }
}





vtkDataSet *avtamrexFileFormat::GetMesh(int timeState, int domain,
                                          const char *visit_meshname) {
  const char *meshName = visit_meshname != nullptr ? visit_meshname : "<null>";
  debug1 << "[amrex-plugin] GetMesh timeState=" << timeState
         << " domain=" << domain << " mesh=" << meshName << "\n";

  if (visit_meshname == nullptr) {
    debug1 << "[amrex-plugin] GetMesh received null mesh name\n";
    EXCEPTION1(InvalidVariableException, meshName);
  }

  EnsureHierarchyInitialized(timeState);

  auto meshTypeIt = meshMap_.find(visit_meshname);
  if (meshTypeIt == meshMap_.end()) {
    debug1 << "[amrex-plugin] GetMesh missing mesh '" << meshName << "'\n";
    EXCEPTION1(InvalidVariableException, visit_meshname);
  }

  auto &hierarchyMap = meshHierarchyCache_.at(timeState);
  DatasetType datasetType = std::get<0>(meshTypeIt->second);

  if (datasetType == DatasetType::Field) {
    auto hierarchyMapIt = hierarchyMap.find(visit_meshname);
    if (hierarchyMapIt == hierarchyMap.end()) {
      debug1 << "[amrex-plugin] GetMesh missing mesh '" << meshName << "'\n";
      EXCEPTION1(InvalidVariableException, visit_meshname);
    }

    const MeshPatchHierarchy &hierarchy = hierarchyMapIt->second;
    if (domain < 0 || domain >= static_cast<int>(hierarchy.patches.size())) {
      debug1 << "[amrex-plugin] GetMesh invalid domain index " << domain
             << " for mesh '" << meshName << "'\n";
      EXCEPTION1(InvalidVariableException, visit_meshname);
    }

    const PatchInfo &patch = hierarchy.patches.at(domain);
    debug5 << "[amrex-plugin] GetMesh domain=" << domain
           << " level=" << patch.level << " using mesh " << patch.meshName
           << "\n";

    std::ostringstream ctx;
    ctx << "GetMesh returning patch domain=" << domain;
    LogPatchSummary(patch, ctx.str());

    vtkDataSet *grid = CreateRectilinearPatch(patch);
    if (auto *rect = vtkRectilinearGrid::SafeDownCast(grid)) {
      AddGhostZonesForPatch(hierarchy, domain, rect);
    }
    debug1 << "[amrex-plugin] GetMesh success mesh='" << meshName
           << "' domain=" << domain << "\n";
    return grid;
  }

  const std::string &speciesName = std::get<1>(meshTypeIt->second);
  auto &speciesMap = particleSpeciesCache_.at(timeState);
  auto speciesIt = speciesMap.find(speciesName);
  if (speciesIt == speciesMap.end()) {
    debug1 << "[amrex-plugin] GetMesh missing particle species '"
           << speciesName << "'\n";
    EXCEPTION1(InvalidVariableException, visit_meshname);
  }

  auto hierarchyMapIt = hierarchyMap.find(visit_meshname);
  if (hierarchyMapIt == hierarchyMap.end()) {
    debug1 << "[amrex-plugin] GetMesh missing hierarchy for particle mesh '"
           << meshName << "'\n";
    EXCEPTION1(InvalidVariableException, visit_meshname);
  }

  const MeshPatchHierarchy &hierarchy = hierarchyMapIt->second;
  vtkDataSet *particleMesh =
      GetParticleMesh(timeState, domain, speciesIt->second, hierarchy);
  debug1 << "[amrex-plugin] GetMesh success particle mesh='"
         << meshName << "' domain=" << domain << "\n";
  return particleMesh;
}





// ****************************************************************************
//  Method: avtamrexFileFormat::GetVar
//
//  Purpose:
//      Gets a scalar variable associated with this file.  Although VTK has
//      support for many different types, the best bet is vtkFloatArray, since
//      that is supported everywhere through VisIt.
//
//  Arguments:
//      timestate  The index of the timestate.  If GetNTimesteps returned
//                 'N' time steps, this is guaranteed to be between 0 and N-1.
//      varname    The name of the variable requested.
//
//  Programmer: benwibking -- generated by xml2avt
//  Creation:   Fri Dec 6 17:16:49 PST 2024
//
// ****************************************************************************

vtkDataArray *avtamrexFileFormat::GetVar(int timeState, int domain,
                                           const char *varname) {
  const char *requestedVar = varname != nullptr ? varname : "<null>";
  debug1 << "[amrex-plugin] GetVar timeState=" << timeState
         << " domain=" << domain << " var=" << requestedVar << "\n";
  if (varname == nullptr) {
    debug1 << "[amrex-plugin] GetVar received null var name\n";
    EXCEPTION1(InvalidVariableException, requestedVar);
  }

  EnsureHierarchyInitialized(timeState);

  auto particleVarIt = particleVarMap_.find(requestedVar);
  if (particleVarIt != particleVarMap_.end()) {
    const ParticleVarInfo &varInfo = particleVarIt->second;
    auto &speciesMap = particleSpeciesCache_.at(timeState);
    auto speciesIt = speciesMap.find(varInfo.speciesName);
    if (speciesIt == speciesMap.end()) {
      debug1 << "[amrex-plugin] GetVar missing particle species '"
             << varInfo.speciesName << "'\n";
      EXCEPTION1(InvalidVariableException, varname);
    }
    auto &hierarchyMap = meshHierarchyCache_.at(timeState);
    auto hierarchyIt = hierarchyMap.find(varInfo.meshName);
    if (hierarchyIt == hierarchyMap.end()) {
      debug1 << "[amrex-plugin] GetVar missing hierarchy for particle mesh '"
             << varInfo.meshName << "'\n";
      EXCEPTION1(InvalidVariableException, varname);
    }
    return GetParticleVar(timeState, domain, varInfo, hierarchyIt->second);
  }

  auto varIt = varMap_.find(varname);
  if (varIt == varMap_.end()) {
    debug1 << "[amrex-plugin] GetVar missing var '" << requestedVar << "'\n";
    EXCEPTION1(InvalidVariableException, varname);
  }

  const std::string &visitMeshName = std::get<0>(varIt->second);
  const std::string &component = std::get<1>(varIt->second);

  auto &hierarchyMap = meshHierarchyCache_.at(timeState);
  auto meshIt = hierarchyMap.find(visitMeshName);
  if (meshIt == hierarchyMap.end()) {
    debug1 << "[amrex-plugin] GetVar missing hierarchy for mesh '"
           << visitMeshName << "'\n";
    EXCEPTION1(InvalidVariableException, visitMeshName.c_str());
  }

  const MeshPatchHierarchy &hierarchy = meshIt->second;
  if (domain < 0 || domain >= static_cast<int>(hierarchy.patches.size())) {
    debug1 << "[amrex-plugin] GetVar invalid domain index " << domain
           << " for mesh '" << visitMeshName << "'\n";
    EXCEPTION1(InvalidVariableException, visitMeshName.c_str());
  }

  const PatchInfo &patch = hierarchy.patches.at(domain);
  std::ostringstream ctx;
  ctx << "GetVar patch domain=" << domain << " component=" << component;
  LogPatchSummary(patch, ctx.str());

  vtkDataArray *data = LoadScalarPatchData(timeState, patch, component);
  if (data == nullptr) {
    debug1 << "[amrex-plugin] GetVar LoadScalarPatchData returned nullptr"
           << " for var '" << requestedVar << "'\n";
    EXCEPTION1(InvalidVariableException, varname);
  }

  debug1 << "[amrex-plugin] GetVar success var='" << requestedVar << "'\n";

  return data;
}

// ****************************************************************************
//  Method: avtamrexFileFormat::GetVectorVar
//
//  Purpose:
//      Gets a vector variable associated with this file.  Although VTK has
//      support for many different types, the best bet is vtkFloatArray, since
//      that is supported everywhere through VisIt.
//
//  Arguments:
//      timestate  The index of the timestate.  If GetNTimesteps returned
//                 'N' time steps, this is guaranteed to be between 0 and N-1.
//      varname    The name of the variable requested.
//
//  Programmer: benwibking -- generated by xml2avt
//  Creation:   Fri Dec 6 17:16:49 PST 2024
//
// ****************************************************************************

vtkDataArray *avtamrexFileFormat::GetVectorVar(int timeState, int domain,
                                                 const char *varname) {
  const char *requestedVar = varname != nullptr ? varname : "<null>";
  debug1 << "[amrex-plugin] GetVectorVar timeState=" << timeState
         << " domain=" << domain << " var=" << requestedVar << "\n";

  if (varname == nullptr) {
    debug1 << "[amrex-plugin] GetVectorVar received null var name\n";
    EXCEPTION1(InvalidVariableException, requestedVar);
  }

  EnsureHierarchyInitialized(timeState);

  auto particleVarIt = particleVectorVarMap_.find(requestedVar);
  if (particleVarIt != particleVectorVarMap_.end()) {
    const ParticleVectorVarInfo &varInfo = particleVarIt->second;
    auto &speciesMap = particleSpeciesCache_.at(timeState);
    auto speciesIt = speciesMap.find(varInfo.speciesName);
    if (speciesIt == speciesMap.end()) {
      debug1 << "[amrex-plugin] GetVectorVar missing particle species '"
             << varInfo.speciesName << "'\n";
      EXCEPTION1(InvalidVariableException, varname);
    }
    auto &hierarchyMap = meshHierarchyCache_.at(timeState);
    auto hierarchyIt = hierarchyMap.find(varInfo.meshName);
    if (hierarchyIt == hierarchyMap.end()) {
      debug1 << "[amrex-plugin] GetVectorVar missing hierarchy for particle mesh '"
             << varInfo.meshName << "'\n";
      EXCEPTION1(InvalidVariableException, varname);
    }
    return GetParticleVectorVar(timeState, domain, varInfo,
                                hierarchyIt->second);
  }

  auto varIt = vectorVarMap_.find(varname);
  if (varIt == vectorVarMap_.end()) {
    debug1 << "[amrex-plugin] GetVectorVar missing var '" << requestedVar << "'\n";
    EXCEPTION1(InvalidVariableException, varname);
  }

  const std::string &visitMeshName = std::get<0>(varIt->second);
  const std::vector<std::string> &components = std::get<1>(varIt->second);

  auto &hierarchyMap = meshHierarchyCache_.at(timeState);
  auto meshIt = hierarchyMap.find(visitMeshName);
  if (meshIt == hierarchyMap.end()) {
    debug1 << "[amrex-plugin] GetVectorVar missing hierarchy for mesh '"
           << visitMeshName << "'\n";
    EXCEPTION1(InvalidVariableException, visitMeshName.c_str());
  }

  const MeshPatchHierarchy &hierarchy = meshIt->second;
  if (domain < 0 || domain >= static_cast<int>(hierarchy.patches.size())) {
    debug1 << "[amrex-plugin] GetVectorVar invalid domain index "
           << domain << " for mesh '" << visitMeshName << "'\n";
    EXCEPTION1(InvalidVariableException, visitMeshName.c_str());
  }

  const PatchInfo &patch = hierarchy.patches.at(domain);
  std::ostringstream ctx;
  ctx << "GetVectorVar patch domain=" << domain
      << " components=" << JoinStrings(components);
  LogPatchSummary(patch, ctx.str());

  vtkDataArray *data = LoadVectorPatchData(timeState, patch, components);
  if (data == nullptr) {
    debug1 << "[amrex-plugin] GetVectorVar LoadVectorPatchData returned"
           << " nullptr for var '" << requestedVar << "'\n";
    EXCEPTION1(InvalidVariableException, varname);
  }

  debug1 << "[amrex-plugin] GetVectorVar success var='" << requestedVar << "'\n";

  return data;
}
