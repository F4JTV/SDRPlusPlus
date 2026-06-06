cmake_minimum_required(VERSION 3.13)
project(drm_decoder)

# ---------------------------------------------------------------------------
# Digital Radio Mondiale (DRM) decoder module for SDR++
#
# The DRM demodulation/decoding chain is provided by the Dream receiver core
# (https://github.com/rafael2k/dream, GPL-2), vendored under ./dream. Clone it
# into this directory before building:
#
#     git clone https://github.com/rafael2k/dream.git
#
# Only the receiver core is compiled (no Qt GUI, no transmitter front-end).
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DREAM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dream")

if (NOT EXISTS "${DREAM_DIR}/src/DrmReceiver.cpp")
    message(FATAL_ERROR
        "Dream sources not found in ${DREAM_DIR}.\n"
        "Run:  git clone https://github.com/rafael2k/dream.git  inside this module directory.")
endif ()

# Dream receiver core (subset of dream.pro common SOURCES, no Qt GUI)
set(DREAM_SRC
    ${DREAM_DIR}/src/AMDemodulation.cpp
    ${DREAM_DIR}/src/AMSSDemodulation.cpp
    ${DREAM_DIR}/src/chanest/ChanEstTime.cpp
    ${DREAM_DIR}/src/chanest/ChannelEstimation.cpp
    ${DREAM_DIR}/src/chanest/IdealChannelEstimation.cpp
    ${DREAM_DIR}/src/chanest/TimeLinear.cpp
    ${DREAM_DIR}/src/chanest/TimeWiener.cpp
    ${DREAM_DIR}/src/creceivedata.cpp
    ${DREAM_DIR}/src/ctransmitdata.cpp
    ${DREAM_DIR}/src/datadecoding/DABMOT.cpp
    ${DREAM_DIR}/src/datadecoding/DataDecoder.cpp
    ${DREAM_DIR}/src/datadecoding/DataEncoder.cpp
    ${DREAM_DIR}/src/datadecoding/epgutil.cpp
    ${DREAM_DIR}/src/datadecoding/Experiment.cpp
    ${DREAM_DIR}/src/datadecoding/Journaline.cpp
    ${DREAM_DIR}/src/datadecoding/journaline/crc_8_16.c
    ${DREAM_DIR}/src/datadecoding/journaline/dabdgdec_impl.c
    ${DREAM_DIR}/src/datadecoding/journaline/log.c
    ${DREAM_DIR}/src/datadecoding/journaline/newsobject.cpp
    ${DREAM_DIR}/src/datadecoding/journaline/newssvcdec_impl.cpp
    ${DREAM_DIR}/src/datadecoding/journaline/NML.cpp
    ${DREAM_DIR}/src/datadecoding/journaline/Splitter.cpp
    ${DREAM_DIR}/src/datadecoding/MOTSlideShow.cpp
    ${DREAM_DIR}/src/DataIO.cpp
    ${DREAM_DIR}/src/drmchannel/ChannelSimulation.cpp
    ${DREAM_DIR}/src/DrmReceiver.cpp
    ${DREAM_DIR}/src/DrmSimulation.cpp
    ${DREAM_DIR}/src/DrmTransmitter.cpp
    ${DREAM_DIR}/src/FAC/FAC.cpp
    ${DREAM_DIR}/src/InputResample.cpp
    ${DREAM_DIR}/src/interleaver/BlockInterleaver.cpp
    ${DREAM_DIR}/src/interleaver/SymbolInterleaver.cpp
    ${DREAM_DIR}/src/IQInputFilter.cpp
    ${DREAM_DIR}/src/matlib/MatlibSigProToolbox.cpp
    ${DREAM_DIR}/src/matlib/MatlibStdToolbox.cpp
    ${DREAM_DIR}/src/MDI/AFPacketGenerator.cpp
    ${DREAM_DIR}/src/MDI/MDIDecode.cpp
    ${DREAM_DIR}/src/MDI/MDIInBuffer.cpp
    ${DREAM_DIR}/src/MDI/MDIRSCI.cpp
    ${DREAM_DIR}/src/MDI/MDITagItemDecoders.cpp
    ${DREAM_DIR}/src/MDI/MDITagItems.cpp
    ${DREAM_DIR}/src/MDI/PacketSinkFile.cpp
    ${DREAM_DIR}/src/MDI/PacketSocket.cpp
    ${DREAM_DIR}/src/MDI/PacketSourceFile.cpp
    ${DREAM_DIR}/src/MDI/Pft.cpp
    ${DREAM_DIR}/src/MDI/RCITagItems.cpp
    ${DREAM_DIR}/src/MDI/RSCITagItemDecoders.cpp
    ${DREAM_DIR}/src/MDI/RSISubscriber.cpp
    ${DREAM_DIR}/src/MDI/TagPacketDecoder.cpp
    ${DREAM_DIR}/src/MDI/TagPacketDecoderMDI.cpp
    ${DREAM_DIR}/src/MDI/TagPacketDecoderRSCIControl.cpp
    ${DREAM_DIR}/src/MDI/TagPacketGenerator.cpp
    ${DREAM_DIR}/src/mlc/BitInterleaver.cpp
    ${DREAM_DIR}/src/mlc/ChannelCode.cpp
    ${DREAM_DIR}/src/mlc/ConvEncoder.cpp
    ${DREAM_DIR}/src/mlc/EnergyDispersal.cpp
    ${DREAM_DIR}/src/mlc/Metric.cpp
    ${DREAM_DIR}/src/mlc/MLC.cpp
    ${DREAM_DIR}/src/mlc/QAMMapping.cpp
    ${DREAM_DIR}/src/mlc/TrellisUpdateMMX.cpp
    ${DREAM_DIR}/src/mlc/TrellisUpdateSSE2.cpp
    ${DREAM_DIR}/src/mlc/ViterbiDecoder.cpp
    ${DREAM_DIR}/src/MSCMultiplexer.cpp
    ${DREAM_DIR}/src/ofdmcellmapping/CellMappingTable.cpp
    ${DREAM_DIR}/src/ofdmcellmapping/OFDMCellMapping.cpp
    ${DREAM_DIR}/src/OFDM.cpp
    ${DREAM_DIR}/src/Parameter.cpp
    ${DREAM_DIR}/src/PlotManager.cpp
    ${DREAM_DIR}/src/ReceptLog.cpp
    ${DREAM_DIR}/src/resample/Resample.cpp
    ${DREAM_DIR}/src/resample/ResampleFilter.cpp
    ${DREAM_DIR}/src/Scheduler.cpp
    ${DREAM_DIR}/src/SDC/SDCReceive.cpp
    ${DREAM_DIR}/src/SDC/SDCTransmit.cpp
    ${DREAM_DIR}/src/SDC/audioparam.cpp
    ${DREAM_DIR}/src/ServiceInformation.cpp
    ${DREAM_DIR}/src/SimulationParameters.cpp
    ${DREAM_DIR}/src/sound/audiofilein.cpp
    # NOTE: aac_codec.cpp is intentionally NOT listed here; we compile a PATCHED
    # copy from dream_patches/ instead (upstream leaves eDecError uninitialised
    # on the success path, which makes the decoder report every frame as
    # broken on some builds, resulting in lock + silence).
    ${DREAM_DIR}/src/sourcedecoders/AudioCodec.cpp
    ${DREAM_DIR}/src/sourcedecoders/AudioSourceDecoder.cpp
    ${DREAM_DIR}/src/sourcedecoders/AudioSourceEncoder.cpp
    ${DREAM_DIR}/src/sourcedecoders/null_codec.cpp
    ${DREAM_DIR}/src/sourcedecoders/opus_codec.cpp
    ${DREAM_DIR}/src/spectrumanalyser.cpp
    ${DREAM_DIR}/src/sync/FreqSyncAcq.cpp
    ${DREAM_DIR}/src/sync/SyncUsingPil.cpp
    ${DREAM_DIR}/src/sync/TimeSync.cpp
    ${DREAM_DIR}/src/sync/TimeSyncFilter.cpp
    ${DREAM_DIR}/src/sync/TimeSyncTrack.cpp
    ${DREAM_DIR}/src/tables/TableCarMap.cpp
    ${DREAM_DIR}/src/tables/TableFAC.cpp
    ${DREAM_DIR}/src/tables/TableStations.cpp
    ${DREAM_DIR}/src/TextMessage.cpp
    ${DREAM_DIR}/src/util/CRC.cpp
    ${DREAM_DIR}/src/util/FileTyper.cpp
    ${DREAM_DIR}/src/util/LogPrint.cpp
    ${DREAM_DIR}/src/util/Reassemble.cpp
    ${DREAM_DIR}/src/util/Settings.cpp
    ${DREAM_DIR}/src/util/Utilities.cpp
    ${DREAM_DIR}/src/Version.cpp
    ${DREAM_DIR}/src/sound/soundnull.cpp
    ${DREAM_DIR}/src/DrmTransceiver.cpp
    ${DREAM_DIR}/src/MSC/logicalframe.cpp
    ${DREAM_DIR}/src/MSC/audiosuperframe.cpp
    ${DREAM_DIR}/src/MSC/aacsuperframe.cpp
    ${DREAM_DIR}/src/MSC/xheaacsuperframe.cpp
    ${DREAM_DIR}/src/MSC/frameborderdescription.cpp
    ${DREAM_DIR}/src/resample/speexresampler.cpp
    ${DREAM_DIR}/src/resample/cspectrumresample.cpp
    ${DREAM_DIR}/src/resample/caudioresample.cpp
    ${DREAM_DIR}/src/sourcedecoders/reverb.cpp
    ${DREAM_DIR}/src/sourcedecoders/caudioreverb.cpp
    ${DREAM_DIR}/src/tuner.cpp
    ${DREAM_DIR}/src/sound/soundfactory.cpp
)

# xHE-AAC (USAC) decoding requires libfdk-aac. Modern DRM broadcasts (e.g. the
# TDF/Issoudun tests) use xHE-AAC, which libfaad2 CANNOT decode: the receiver
# will lock and show the station label but produce no audio. Enable this when
# libfdk-aac is available to get sound on those stations.
option(OPT_DRM_USE_FDK_AAC "Build the DRM decoder with libfdk-aac for xHE-AAC support" ON)

find_package(PkgConfig REQUIRED)

set(DREAM_FDK_SRC "")
set(DRM_HAVE_FDK FALSE)
if (OPT_DRM_USE_FDK_AAC)
    pkg_check_modules(FDKAAC fdk-aac)
    if (FDKAAC_FOUND)
        set(DRM_HAVE_FDK TRUE)
        # Use our PATCHED copy of the FDK codec (see dream_patches/): it limits
        # FDK to xHE-AAC only and leaves classic AAC/HE-AAC to libfaad2, because
        # the system libfdk-aac advertises DRM AAC support but actually fails on
        # real DRM AAC streams ("zero output channels"). libfaad2 handles those.
        list(APPEND DREAM_FDK_SRC ${CMAKE_CURRENT_SOURCE_DIR}/dream_patches/fdk_aac_codec.cpp)
        message(STATUS "drm_decoder: xHE-AAC enabled via libfdk-aac (${FDKAAC_LIBRARIES})")
    else ()
        message(WARNING "drm_decoder: OPT_DRM_USE_FDK_AAC is ON but libfdk-aac was not found. "
                        "xHE-AAC stations (e.g. TDF) will lock but stay silent. "
                        "Install libfdk-aac-dev, or set -DOPT_DRM_USE_FDK_AAC=OFF.")
    endif ()
endif ()

# Module sources + Dream backend compiled into a single shared object
file(GLOB MODULE_SRC "src/*.cpp")
# Always include the patched AAC codec (fixes uninitialised return value bug
# in upstream Dream that caused TDF DRM and similar AAC streams to be silent).
set(DREAM_PATCHED_SRC ${CMAKE_CURRENT_SOURCE_DIR}/dream_patches/aac_codec.cpp)
set(SRC ${MODULE_SRC} ${DREAM_SRC} ${DREAM_PATCHED_SRC} ${DREAM_FDK_SRC})

include(${SDRPP_MODULE_CMAKE})

# ---- Dream backend configuration ----
# NO_QT             : build the receiver core without the Qt GUI/util layer
# USE_FAAD2_LIBRARY : link libfaad2 directly (it exports NeAACDecInitDRM) instead
#                     of Dream's runtime dlopen of a separate libfaad_drm
# USE_OPUS_LIBRARY  : link libopus directly
target_compile_definitions(drm_decoder PRIVATE NO_QT USE_FAAD2_LIBRARY USE_OPUS_LIBRARY)

target_include_directories(drm_decoder PRIVATE
    "src/"
    "${DREAM_DIR}"
    "${DREAM_DIR}/src"
    "${DREAM_DIR}/src/sourcedecoders"
)

if (MSVC)
    target_compile_options(drm_decoder PRIVATE /O2 /EHsc /bigobj)
else ()
    target_compile_options(drm_decoder PRIVATE -O3 -Wno-unused-parameter -Wno-deprecated-declarations)
endif ()

# ---- Backend dependencies ----
pkg_check_modules(FFTW3F  REQUIRED fftw3f)
pkg_check_modules(FFTW3   REQUIRED fftw3)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(SPEEXDSP REQUIRED speexdsp)
pkg_check_modules(OPUS    REQUIRED opus)

target_include_directories(drm_decoder PRIVATE
    ${FFTW3F_INCLUDE_DIRS} ${FFTW3_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS} ${SPEEXDSP_INCLUDE_DIRS} ${OPUS_INCLUDE_DIRS}
)

# xHE-AAC via libfdk-aac (detected earlier as DRM_HAVE_FDK).
set(FDK_LIBRARY "")
if (DRM_HAVE_FDK)
    # HAVE_LIBFDK_AAC registers the FDK codec; HAVE_USAC enables its xHE-AAC
    # (USAC) path. Both are needed, matching Dream's own dream.pro.
    target_compile_definitions(drm_decoder PRIVATE HAVE_LIBFDK_AAC HAVE_USAC)
    target_include_directories(drm_decoder PRIVATE
        ${FDKAAC_INCLUDE_DIRS}
        "${DREAM_DIR}/src/sourcedecoders")
    set(FDK_LIBRARY ${FDKAAC_LIBRARIES})
endif ()

# DRM needs the faad2 build that exports NeAACDecInitDRM. On Debian/Ubuntu the
# libfaad-dev package ships TWO libraries:
#   libfaad.so      - plain AAC, WITHOUT NeAACDecInitDRM
#   libfaad_drm.so  - same API plus the NeAACDecInitDRM entry point DRM needs
# Linking plain -lfaad makes the module load fine and show SNR but fail the
# moment it tries to start audio decode. Prefer libfaad_drm, fall back to faad.
if (NOT MSVC)
    find_library(FAAD_DRM_LIB NAMES faad_drm)
    find_library(FAAD_LIB     NAMES faad)
    if (FAAD_DRM_LIB)
        set(FAAD_LIBRARY ${FAAD_DRM_LIB})
        message(STATUS "drm_decoder: using ${FAAD_DRM_LIB} (has NeAACDecInitDRM)")
    elseif (FAAD_LIB)
        set(FAAD_LIBRARY ${FAAD_LIB})
        message(WARNING "drm_decoder: libfaad_drm not found, falling back to ${FAAD_LIB}. "
                        "If DRM audio fails to decode, install a faad2 build with DRM support.")
    else ()
        message(FATAL_ERROR "drm_decoder: neither libfaad_drm nor libfaad found. Install libfaad-dev.")
    endif ()
else ()
    set(FAAD_LIBRARY faad)
endif ()

target_link_libraries(drm_decoder PRIVATE
    ${FFTW3F_LIBRARIES} ${FFTW3_LIBRARIES}
    ${SNDFILE_LIBRARIES} ${SPEEXDSP_LIBRARIES} ${OPUS_LIBRARIES}
    ${FAAD_LIBRARY} ${FDK_LIBRARY} z pcap ${CMAKE_DL_LIBS}
)
