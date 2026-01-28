if(NOT DEFINED AMREX_SOURCE_DIR)
  message(FATAL_ERROR "AMREX_SOURCE_DIR not set for AMReX patching.")
endif()
if(NOT DEFINED AMREX_PATCH_STAMP)
  message(FATAL_ERROR "AMREX_PATCH_STAMP not set for AMReX patching.")
endif()

set(_amrex_source_dir "${AMREX_SOURCE_DIR}")
string(REPLACE "\"" "" _amrex_source_dir "${_amrex_source_dir}")
set(_amrex_header "${_amrex_source_dir}/Src/Base/AMReX_PlotFileDataImpl.H")
if(NOT EXISTS "${_amrex_header}")
  message(FATAL_ERROR "AMReX header not found: ${_amrex_header}")
endif()

file(READ "${_amrex_header}" _amrex_header_contents)
string(FIND "${_amrex_header_contents}" "class avtamrexFileFormat;" _decl_pos)
string(FIND "${_amrex_header_contents}" "friend class ::avtamrexFileFormat;" _friend_pos)

set(_amrex_header_updated "${_amrex_header_contents}")
if(_decl_pos LESS 0)
  string(REPLACE "#include <string>\n\nnamespace amrex {"
                 "#include <string>\n\nclass avtamrexFileFormat;\n\nnamespace amrex {"
                 _amrex_header_updated "${_amrex_header_updated}")
endif()

if(_friend_pos LESS 0)
  string(REPLACE "private:\n    std::string m_plotfile_name;"
                 "private:\n    friend class ::avtamrexFileFormat;\n    std::string m_plotfile_name;"
                 _amrex_header_updated "${_amrex_header_updated}")
endif()

if(NOT _amrex_header_updated STREQUAL _amrex_header_contents)
  file(WRITE "${_amrex_header}" "${_amrex_header_updated}")
endif()

file(TOUCH "${AMREX_PATCH_STAMP}")
