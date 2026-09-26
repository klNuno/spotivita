cmake_minimum_required(VERSION 3.10)

set(VITA_APP_NAME "Spotivita")
# Own TITLEID so Spotivita installs alongside the old CSpot (CSPOT0021), not over it.
set(VITA_TITLEID  "SPOTIVITA")
set(VITA_VERSION  "01.00")

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(spotivita C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11")
# GCC >= 13 stopped pulling <cstdint> in transitively; the pinned cspot/bell
# sources rely on it, so force it instead of patching every submodule file.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++20 -include cstdint")
set(CMAKE_CXX_STANDARD 20)

# libstdc++ decides at runtime whether threads exist by checking a WEAK reference
# to pthread_cancel (gthr-posix.h, non-glibc branch). In a static Vita link nothing
# pulls pthread_cancel in, the weak ref resolves to 0, and every std::mutex /
# std::lock_guard / std::condition_variable silently becomes a no-op (and
# std::thread throws "Enable multithreading"). That broke the audio ring buffer,
# PlayerModel and cspot's Mercury/Shannon locking. Force the symbol in.
# The rest of gthr-posix.h is weak too: once threads count as active, nothing
# pulled pthread_cond_* or pthread_once in either, so std::condition_variable
# and std::call_once jumped to address 0 (found under Vita3K). Force them all;
# check with `nm spotivita | grep " w pthread"`, only the stack hook may stay.
foreach(_sym pthread_cancel pthread_once pthread_cond_wait pthread_cond_timedwait
             pthread_cond_signal pthread_cond_broadcast pthread_cond_destroy)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--undefined=${_sym}")
endforeach()

# --- Patch the pinned cspot/bell submodule for the Vita toolchain ---------
# The cspot submodule is pinned at an old commit; these overrides are applied
# in-place at configure time so the checkout itself stays otherwise clean.

# Full-file Vita replacements copied over the submodule. The copies happen at
# configure time, so an edit under patch/ must re-run configure.
file(GLOB_RECURSE _patch_files ${CMAKE_CURRENT_SOURCE_DIR}/patch/*)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_patch_files})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/Queue.h
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/bell/include)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/ZeroconfAuthenticator.cpp
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/src)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/ZeroconfAuthenticator.h
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/include)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/ApResolve.cpp
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/src)
# TCPSocket.h includes <sys/ioctl.h> (absent on Vita); guarded copy.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/TCPSocket.h
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/bell/include)
# NanoPBHelper with the HTTPClient/TLS helpers stripped (unused, OpenSSL-only).
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/NanoPBHelper.h
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/bell/include)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/NanoPBHelper.cpp
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/bell/src)
# Forked bell CMakeLists: selects the mbedtls crypto backend and drops the HTTP
# client stack on Vita (vitasdk OpenSSL is 1.0.2, too old for CryptoOpenSSL.cpp).
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/bell/CMakeLists.txt
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/bell)
# bell's HTTPServer::listen() is select-based; vitasdk's select() never reports
# the sceNet listen socket readable, so it never accept()ed and every Zeroconf
# request hung with 0 bytes. This replacement uses a blocking accept() loop.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/bell/HTTPServer.cpp
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/bell/src)
# Shannon MAC mismatch = desynced stream cipher. Upstream logged and kept
# reading garbage in a tight loop (starved the GUI mid-GPU-frame and wedged the
# whole console). The patch throws so MercuryManager's existing reconnection
# path rebuilds the session cleanly.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/ShannonConnection.cpp
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/src)
# Reconnect path is exception-unsafe: recvPacket/reconnectedCallback (spirc
# subscribe) throw std::runtime_error on a blipped AP link, and the throw
# unwinds out of the recv thread -> std::terminate -> abort (coredump-confirmed
# _kill_r/__verbose_terminate_handler crash). The patch broadens the catch and
# guards reconnectedCallback so a reconnect never crashes the process.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/MercuryManager.cpp
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/src)
# Local playback (playTracks, seek, shuffle, repeat): the Web API player
# endpoints answer 429 to tokens minted for this client, so the Vita builds its
# own queue instead of asking Spotify's servers to start playback.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/SpircController.h
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/include)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/SpircController.cpp
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/src)
# Audio file choice: the configured bitrate, then any other Vorbis file, instead
# of stalling silently when a track lacks the configured one.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/SpotifyTrack.cpp
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/src)
# A reconnect fails every pending audio chunk. Upstream kept reading the failed
# chunk in a busy loop (silence, controls starved); these re-request it, end an
# unreadable stream like a finished track, and lock currentTrack, which the
# player task deletes while the spirc thread pauses or cancels it.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/ChunkedByteStream.cpp
          ${CMAKE_CURRENT_SOURCE_DIR}/patch/ChunkedAudioStream.cpp
          ${CMAKE_CURRENT_SOURCE_DIR}/patch/Player.cpp
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/src)

# Surgical edit to the pinned cspot CMakeLists (bell is replaced wholesale above).
#  1) CMake >= 3.27 hard-errors on cmake_minimum_required < 3.5.
#  2) cspot links Apple Bonjour 'dns_sd' on the UNIX-but-not-Apple branch;
#     vitasdk ships no such lib (mDNS is done by hand in ZeroconfAuthenticator).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/CMakeLists.txt" _c)
string(REGEX REPLACE "cmake_minimum_required\\(VERSION[^)]*\\)"
                     "cmake_minimum_required(VERSION 3.10)" _c "${_c}")
string(REPLACE "list(APPEND EXTRA_LIBS dns_sd)"
               "# list(APPEND EXTRA_LIBS dns_sd) # disabled for Vita (no Bonjour)"
               _c "${_c}")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/cspot/cspot/CMakeLists.txt" "${_c}")

# add CSpot sources
# mbedtls backend: vitasdk OpenSSL (1.0.2) lacks the 1.1+ API CryptoOpenSSL.cpp
# needs, so bell uses CryptoMbedTLS instead (see patch/bell/CMakeLists.txt).
add_compile_definitions(BELL_USE_MBEDTLS)
add_compile_definitions(VITA)
# Spotify streams OGG Vorbis (decoded by tremor); the bundled libhelix MP3/AAC
# codecs have no Vita assembly path (assembly.h: "Unsupported platform").
set(BELL_DISABLE_CODECS ON CACHE BOOL "no libhelix MP3/AAC on Vita" FORCE)
add_subdirectory("cspot/cspot" ${CMAKE_CURRENT_BINARY_DIR}/cspot)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
# vdpm's CURLTargets.cmake hardcodes /usr/local/vitasdk for libzstd, which breaks
# any SDK installed elsewhere (e.g. the native Windows toolchain). Relocate it.
if(TARGET CURL::libcurl_static)
  get_target_property(_curl_deps CURL::libcurl_static INTERFACE_LINK_LIBRARIES)
  if(_curl_deps)
    string(REPLACE "/usr/local/vitasdk" "${VITASDK}" _curl_deps "${_curl_deps}")
    set_target_properties(CURL::libcurl_static PROPERTIES INTERFACE_LINK_LIBRARIES "${_curl_deps}")
  endif()
endif()

include_directories(
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/cspot/cspot/include
  ${CMAKE_SOURCE_DIR}/cspot/cspot/bell/include
)

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/API.cpp
  src/Login5.cpp
  src/Utils.cpp
  src/Keyboard.cpp
  src/CliFile.cpp
  src/VitaAudioSink.cpp
  src/CircularBuffer.cpp
  src/Gui.cpp
  src/PlaybackScreen.cpp
  src/LoginScreen.cpp
  src/GuiUtils.cpp
  src/Input.cpp
  src/Render.cpp
  src/NetWorker.cpp
  src/DevKit.cpp
  src/Compat.c
)

# Remote-control test server (see include/DevKit.h). It reads and writes files
# over the LAN, so it is off unless a dev build asks for it.
option(SPOTIVITA_DEVKIT "Build the port 2138 debug server" OFF)
if(SPOTIVITA_DEVKIT)
  target_compile_definitions(${PROJECT_NAME} PRIVATE SPOTIVITA_DEVKIT)
endif()

target_link_libraries(${PROJECT_NAME}
  SceBgAppUtil_stub
  SceNotificationUtil_stub
  pthread
  cspot
  m
  SceAudio_stub
  SceShellSvc_stub
  # Dear ImGui 1.79 core plus the two precompiled GXP shaders (src/Render.cpp).
  imgui_vita2d
  vita2d
  SceAppMgr_stub
  SceAppUtil_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceKernelDmacMgr_stub
  SceLibKernel_stub
  ScePower_stub
  SceSysmodule_stub
  SceTouch_stub
  taihen_stub
  ${CURL_LIBRARIES}
  ${OPENSSL_LIBRARIES}
  z
)

vita_create_self(eboot.bin
  ${PROJECT_NAME}
  UNSAFE
  ATTRIBUTE 0x0000000E
  MEMSIZE 0x0
)

set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE=17338440 -d ATTRIBUTE2=0 -d ATTRIBUTE_MINOR=17 -s CATEGORY=gdc")

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE common_data/sce_sys/icon0.png sce_sys/icon0.png
  FILE common_data/fa-solid-900.ttf fa-solid-900.ttf
  FILE common_data/icon_alpha.png icon_alpha.png
  FILE common_data/PlusJakartaSans-Regular.ttf PlusJakartaSans-Regular.ttf
  FILE common_data/PlusJakartaSans-Medium.ttf PlusJakartaSans-Medium.ttf
  FILE common_data/PlusJakartaSans-Bold.ttf PlusJakartaSans-Bold.ttf
  FILE common_data/Roboto-Regular.ttf Roboto-Regular.ttf
  FILE common_data/Roboto-Bold.ttf Roboto-Bold.ttf
  FILE common_data/cacert.pem cacert.pem
  FILE common_data/cover_art.png cover_art.png
)
