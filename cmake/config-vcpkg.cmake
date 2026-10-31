# Contains options for VCPKG features

option(RTS_BUILD_OPTION_FFMPEG "Enable FFmpeg support" OFF)
if(RTS_BUILD_OPTION_FFMPEG)
    list(APPEND VCPKG_MANIFEST_FEATURES "ffmpeg")
endif()

option(RTS_BUILD_OPTION_OPENAL "Enable OpenAL audio (needs FFmpeg for video)" OFF)
if(RTS_BUILD_OPTION_OPENAL)
    list(APPEND VCPKG_MANIFEST_FEATURES "openal")
endif()
