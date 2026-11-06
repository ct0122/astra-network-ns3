set(astra-sim-ns3-libs-list
  "libapplications"
  "libcore"
  "libcsma"
  "libinternet"
  "libpoint-to-point"
  "libnetwork"
)
build_exec(
  EXECNAME memop-read-smoke
  SOURCE_FILES memop-read-smoke.cc
  LIBRARIES_TO_LINK "${libapplications}" "${libinternet}" "${libpoint-to-point}"
  EXECUTABLE_DIRECTORY_PATH "${PROJECT_SOURCE_DIR}/build/scratch/"
)
option(NS3_MEMOP_FABRIC_ONLY "Build the standalone fabric test without ASTRA dependencies" OFF)
if(NS3_MEMOP_FABRIC_ONLY)
  return()
endif()
list(JOIN astra-sim-ns3-libs-list " " astra-sim-ns3-libs)
set(astra-sim-dir "${PROJECT_SOURCE_DIR}/../../../")
include_directories("../scratch")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../ AstraSim)

build_exec(
  EXECNAME AstraSimNetwork
  SOURCE_FILES ${astra-sim-dir}/astra-sim/network_frontend/ns3/AstraSimNetwork.cc
  DIRECTORY_PATHS ${astra-sim-dir}/extern/helper
  LIBRARIES_TO_LINK "${astra-sim-ns3-libs-list}" "AstraSim"
  EXECUTABLE_DIRECTORY_PATH "${PROJECT_SOURCE_DIR}/build/scratch/"
)
