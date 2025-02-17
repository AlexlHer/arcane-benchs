set(EXAMPLE_NAME Mahyco)
project(Mahyco LANGUAGES C CXX)
include(../mahyco.common.cmake)

target_sources(${EXAMPLE_NAME} PRIVATE MahycoAnnexe.cc)
target_sources(${EXAMPLE_NAME} PRIVATE PrepareRemap.cc)

add_library(PerfectGas eos/perfectgas/PerfectGasEOSService.cc)
target_include_directories(PerfectGas PUBLIC .)
target_link_libraries(PerfectGas PUBLIC arcane_core)

add_library(StiffenedGas eos/stiffenedgas/StiffenedGasEOSService.cc)
target_include_directories(StiffenedGas PUBLIC .)
target_link_libraries(StiffenedGas PUBLIC arcane_core)


add_library(RIDER casTest/RIDER/RIDERService.cc)
target_include_directories(RIDER PUBLIC .)
target_link_libraries(RIDER PUBLIC arcane_core)

add_library(SOD casTest/SOD/SODService.cc)
target_include_directories(SOD PUBLIC .)
target_link_libraries(SOD PUBLIC arcane_core)

add_library(SEDOV casTest/SEDOV/SEDOVService.cc)
target_include_directories(SEDOV PUBLIC .)
target_link_libraries(SEDOV PUBLIC arcane_core)

add_library(OTHER casTest/OTHER/OTHERService.cc)
target_include_directories(OTHER PUBLIC .)
target_link_libraries(OTHER PUBLIC arcane_core)


add_library(Remap Remap/RemapADIService.cc Remap/RemapALEService.cc Remap/Remap-ordre-3.cc Remap/UtilesRemap.cc Remap/RemapADIFinal.cc Remap/UtilesRemapALE.cc Remap/DualPhaseRemap.cc)
target_include_directories(Remap PUBLIC .)
target_link_libraries(Remap PUBLIC arcane_core)

# Cartesian
add_library(cartesian cartesian/CartesianConnectivity.cc
                      cartesian/CartesianMesh.cc
                      cartesian/CellDirectionMng.cc
                      cartesian/FaceDirectionMng.cc
                      cartesian/NodeDirectionMng.cc
                      cartesian/CartesianItemSorter.cc)
target_include_directories(cartesian PUBLIC .)
target_link_libraries(cartesian PUBLIC arcane_core)

# Services communs pour les accélérateurs
add_library(accenv accenv/AccEnvDefaultService.cc)
target_include_directories(accenv PUBLIC .)
target_link_libraries(accenv PUBLIC arcane_core)

# Message Passing
add_library(msgpass msgpass/VarSyncMng.cc 
                    msgpass/GlobalSynchronize.cc
                    msgpass/GlobalSynchronizeQueue.cc
                    msgpass/GlobalSynchronizeQueueEvent.cc
                    msgpass/GlobalSynchronizeQueueEventD.cc
                    msgpass/GlobalSynchronizeDevThr.cc
                    msgpass/GlobalSynchronizeDevQueues.cc
                    msgpass/IncompleteGlobalSynchronizeQueue.cc
                    msgpass/IsCommDeviceAware.cc
                    msgpass/MsgPassInit.cc
                    msgpass/PackTransfer.cc
                    msgpass/SyncItems.cc
                    msgpass/SyncBuffers.cc
                    msgpass/SyncEnvIndexes.cc
                    msgpass/Algo1SyncDataMMatD.cc
                    msgpass/Algo1SyncDataMMatDH.cc
                    msgpass/MeshVariableSynchronizerList.cc
                    msgpass/VarSyncAlgo1.cc)
target_include_directories(msgpass PUBLIC .)
target_link_libraries(msgpass PUBLIC arcane_core)
# Pour MPI
find_package(MPI)
if(MPI_FOUND)
  target_compile_definitions(msgpass PRIVATE OMPI_SKIP_MPICXX)
  target_compile_definitions(msgpass PRIVATE MSG_PASS_HAS_MPI)
  target_include_directories(msgpass PRIVATE "${MPI_INCLUDE_PATH}")
  find_path(P4GPU_MPI_EXT NAMES "mpi-ext.h" PATHS "${MPI_C_HEADER_DIR}")
  if(P4GPU_MPI_EXT)
    target_compile_definitions(msgpass PRIVATE MSG_PASS_HAS_MPI_EXT)
  endif()
endif()
# fin MPI

# Pour la gestion des accélérateurs et des sources à compiler avec le
# compilateur pour accélérateur
arcane_accelerator_enable()
arcane_accelerator_add_source_files(MahycoModule.cc )
arcane_accelerator_add_source_files(MahycoAnnexe.cc)
arcane_accelerator_add_source_files(PrepareRemap.cc )
arcane_accelerator_add_source_files(Remap/DualPhaseRemap.cc)
arcane_accelerator_add_source_files(Remap/RemapADIFinal.cc)
arcane_accelerator_add_source_files(Remap/RemapADIService.cc)
arcane_accelerator_add_source_files(Remap/RemapALEService.cc)
arcane_accelerator_add_source_files(Remap/Remap-ordre-3.cc)
arcane_accelerator_add_source_files(Remap/UtilesRemapALE.cc)
arcane_accelerator_add_source_files(Remap/UtilesRemap.cc)
arcane_accelerator_add_source_files(eos/perfectgas/PerfectGasEOSService.cc)
arcane_accelerator_add_source_files(eos/stiffenedgas/StiffenedGasEOSService.cc)
arcane_accelerator_add_source_files(cartesian/CartesianConnectivity.cc)
arcane_accelerator_add_source_files(cartesian/CartesianMesh.cc)
arcane_accelerator_add_source_files(cartesian/CellDirectionMng.cc)
arcane_accelerator_add_source_files(cartesian/FaceDirectionMng.cc)
arcane_accelerator_add_source_files(cartesian/NodeDirectionMng.cc)
arcane_accelerator_add_source_files(cartesian/CartesianItemSorter.cc)
arcane_accelerator_add_source_files(accenv/AccEnvDefaultService.cc)
arcane_accelerator_add_source_files(msgpass/VarSyncMng.cc)
arcane_accelerator_add_source_files(msgpass/GlobalSynchronize.cc)
arcane_accelerator_add_source_files(msgpass/GlobalSynchronizeQueue.cc)
arcane_accelerator_add_source_files(msgpass/GlobalSynchronizeQueueEvent.cc)
arcane_accelerator_add_source_files(msgpass/GlobalSynchronizeQueueEventD.cc)
arcane_accelerator_add_source_files(msgpass/GlobalSynchronizeDevThr.cc)
arcane_accelerator_add_source_files(msgpass/GlobalSynchronizeDevQueues.cc)
arcane_accelerator_add_source_files(msgpass/IncompleteGlobalSynchronizeQueue.cc)
arcane_accelerator_add_source_files(msgpass/IsCommDeviceAware.cc)
arcane_accelerator_add_source_files(msgpass/MsgPassInit.cc)
arcane_accelerator_add_source_files(msgpass/PackTransfer.cc)
arcane_accelerator_add_source_files(msgpass/SyncItems.cc)
arcane_accelerator_add_source_files(msgpass/SyncEnvIndexes.cc)
arcane_accelerator_add_source_files(msgpass/SyncBuffers.cc)
arcane_accelerator_add_to_target(Mahyco)
arcane_accelerator_add_to_target(Remap)
arcane_accelerator_add_to_target(PerfectGas)
arcane_accelerator_add_to_target(StiffenedGas)
arcane_accelerator_add_to_target(cartesian)
arcane_accelerator_add_to_target(accenv)
arcane_accelerator_add_to_target(msgpass)

# Pour la gestion du profiling sur accélérateur
option(WANT_PROF_ACC "Active le profiling des points d'entrées pour accélérateur" FALSE)
if (WANT_PROF_ACC)
  target_compile_definitions(${EXAMPLE_NAME} PRIVATE PROF_ACC)
  target_compile_definitions(PerfectGas PRIVATE PROF_ACC)
  target_compile_definitions(StiffenedGas PRIVATE PROF_ACC)
  target_compile_definitions(Remap PRIVATE PROF_ACC)
  target_compile_definitions(cartesian PRIVATE PROF_ACC)
  target_compile_definitions(accenv PRIVATE PROF_ACC)
endif()

# Pour utiliser l'implémentation Arcane du cartésien
option(WANT_IMPL_CART_ARCANE "Utilisation de l'implémentation Arcane:: dans arcane/cea/" FALSE)
if (WANT_IMPL_CART_ARCANE)
  target_compile_definitions(${EXAMPLE_NAME} PRIVATE IMPL_CART_ARCANE)
  target_compile_definitions(PerfectGas PRIVATE IMPL_CART_ARCANE)
  target_compile_definitions(StiffenedGas PRIVATE IMPL_CART_ARCANE)
  target_compile_definitions(Remap PRIVATE IMPL_CART_ARCANE)
  target_compile_definitions(cartesian PRIVATE IMPL_CART_ARCANE)
  target_compile_definitions(accenv PRIVATE IMPL_CART_ARCANE)
  # TODO : IMPL_CART_ARCANE est un define de cartesian, pour les autres target
  # ce define pourrait etre recupere en exprimant la dependance de la target avec
  # cartesian
endif()

target_link_libraries(${EXAMPLE_NAME} PRIVATE PerfectGas StiffenedGas RIDER SOD SEDOV OTHER Remap cartesian accenv msgpass)

arcane_target_add_axl(RIDER casTest/RIDER/RIDER)
arcane_target_add_axl(SOD casTest/SOD/SOD)
arcane_target_add_axl(SEDOV casTest/SEDOV/SEDOV)
arcane_target_add_axl(OTHER casTest/OTHER/OTHER)

arcane_target_add_axl(PerfectGas eos/perfectgas/PerfectGasEOS)
arcane_target_add_axl(StiffenedGas eos/stiffenedgas/StiffenedGasEOS)

arcane_target_add_axl(Remap Remap/RemapADI)
arcane_target_add_axl(Remap Remap/RemapALE)

arcane_target_add_axl(accenv accenv/AccEnvDefault)

