cmake_minimum_required(VERSION 3.13)
project(drm_image_decoder)

# Toutes les sources : la coque SDR++ (src/*.cpp) + le moteur DRM
# vendorisé (src/engine/*.cpp), récupérées récursivement.
file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

# En-têtes de la coque et du moteur
target_include_directories(drm_image_decoder PRIVATE "src/" "src/engine/")

if (MSVC)
    # FFTW (acquisition + canal) + OpenJPEG (décodage JP2)
    find_package(FFTW3  CONFIG REQUIRED)
    find_package(FFTW3f CONFIG REQUIRED)
    find_package(OpenJPEG CONFIG REQUIRED)
    target_link_libraries(drm_image_decoder PRIVATE
        FFTW3::fftw3 FFTW3::fftw3f openjp2)
elseif (ANDROID)
    target_link_libraries(drm_image_decoder PUBLIC
        /sdr-kit/${ANDROID_ABI}/lib/libfftw3.so
        /sdr-kit/${ANDROID_ABI}/lib/libfftw3f.so
        /sdr-kit/${ANDROID_ABI}/lib/libopenjp2.so
    )
else ()
    find_package(PkgConfig)
    pkg_check_modules(FFTW3   REQUIRED fftw3)
    pkg_check_modules(FFTW3F  REQUIRED fftw3f)
    pkg_check_modules(OPENJP2 REQUIRED libopenjp2)

    target_include_directories(drm_image_decoder PRIVATE
        ${FFTW3_INCLUDE_DIRS} ${FFTW3F_INCLUDE_DIRS} ${OPENJP2_INCLUDE_DIRS})
    target_link_directories(drm_image_decoder PRIVATE
        ${FFTW3_LIBRARY_DIRS} ${FFTW3F_LIBRARY_DIRS} ${OPENJP2_LIBRARY_DIRS})
    target_link_libraries(drm_image_decoder PRIVATE
        ${FFTW3_LIBRARIES} ${FFTW3F_LIBRARIES} ${OPENJP2_LIBRARIES})

    if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        target_include_directories(drm_image_decoder PRIVATE "/usr/local/include")
    endif()
endif ()

# Note : stb_image / stb_image_write sont des bibliothèques single-header
# distribuées via le paquet `libstb-dev` (Ubuntu) ; elles s'incluent depuis
# /usr/include/stb/stb_image*.h sans linkage supplémentaire.
