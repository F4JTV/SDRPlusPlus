/*
 * engine_stubs.cpp
 *
 * Definitions of the cross-cutting symbols declared in qt_shim.h:
 *
 *   - drmlog::g_sink        : optional engine log sink (set by the module).
 *   - drmcapture::mtx       : mutex protecting captured-file storage.
 *   - drmcapture::files     : buffered "filesystem" capturing what the
 *                              engine wrote via QFile (i.e. assembled images).
 *   - drmcapture::counter   : strictly-increasing counter (UI can poll it).
 *
 * The 'srcDecoder' global (sourceDecoder*) is DEFINED by the vendored
 * drm.cpp and instantiated (new sourceDecoder) by the wrapper in
 * drm_decoder.cpp. Real channel_decoding() lives in channeldecode.cpp;
 * real source_decoding() lives in sourcedecoder.cpp (increment 3 wiring).
 */

#include "qt_shim.h"
#include "shim_deps.h"     // pour drmprogress (déclarations)

namespace drmlog {
    LogSink g_sink = nullptr;
}

namespace drmcapture {
    std::mutex                  mtx;
    std::vector<CapturedFile>   files;
    uint64_t                    counter = 0;
}

namespace drmprogress {
    std::atomic<int>      currentSegments{0};
    std::atomic<int>      totalSegments{0};
    std::atomic<int>      bodySize{0};
    std::atomic<int>      segmentSize{0};
    std::atomic<uint64_t> frameCounter{0};
    std::mutex            nameMtx;
    std::string           objectName;
}
