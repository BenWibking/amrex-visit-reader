// ABOUTME: Generates a single-level AMReX plotfile whose MultiFab is written with ghost cells.
// ABOUTME: Interior cells hold i + 100*j + 10000*k; ghost cells hold -12345 so misreads are detectable.
//
// Build (against the plugin's vendored AMReX build):
//   build/_deps/mpich-install/bin/mpicxx -std=c++17 -O2 \
//     example_data/make_ghost_plotfile.cpp \
//     -I build/_deps/amrex-build -I build/_deps/amrex-src/Src/Base \
//     -I build/_deps/amrex-src/Src/Base/Parser \
//     -L build/_deps/amrex-build/Src -lamrex_3d \
//     -Wl,-rpath,build/_deps/amrex-build/Src \
//     -o build/make_ghost_plotfile
//
// Run: build/make_ghost_plotfile <output-plotfile-dir>

#include <AMReX.H>
#include <AMReX_Geometry.H>
#include <AMReX_MultiFab.H>
#include <AMReX_PlotFileUtil.H>
#include <AMReX_VisMF.H>

int main(int argc, char **argv) {
  // Skip ParmParse setup so argv[1] is not interpreted as an inputs file.
  amrex::Initialize(argc, argv, false);
  {
    std::string plotname = "plt_ghost00000";
    if (argc > 1) {
      plotname = argv[1];
    }

    const int ncell = 16;
    amrex::Box domain(amrex::IntVect(0), amrex::IntVect(ncell - 1));
    amrex::BoxArray ba(domain);
    ba.maxSize(8); // several boxes so patch adjacency is exercised
    amrex::DistributionMapping dm(ba);

    amrex::MultiFab mf(ba, dm, 1, 1); // one component, one ghost cell
    mf.setVal(-12345.0);              // sentinel, survives only in ghosts
    for (amrex::MFIter mfi(mf); mfi.isValid(); ++mfi) {
      const amrex::Box &vbx = mfi.validbox();
      auto arr = mf.array(mfi);
      amrex::LoopOnCpu(vbx, [&](int i, int j, int k) {
        arr(i, j, k) = static_cast<amrex::Real>(i + 100 * j + 10000 * k);
      });
    }

    amrex::RealBox rb({0.0, 0.0, 0.0}, {1.0, 1.0, 1.0});
    amrex::Geometry geom(domain, rb, 0, {0, 0, 0});

    // WriteSingleLevelPlotfile strips ghost cells, so write the plotfile
    // normally and then overwrite the level data with the ghosted MultiFab.
    amrex::WriteSingleLevelPlotfile(plotname, mf, {"testvar"}, geom, 0.0, 0);
    amrex::VisMF::Write(mf, plotname + "/Level_0/Cell");

    amrex::Print() << "Wrote ghosted plotfile " << plotname << "\n";
  }
  amrex::Finalize();
  return 0;
}
