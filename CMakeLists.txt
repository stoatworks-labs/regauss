cmake_minimum_required(VERSION 3.15)

project(Regauss
    VERSION 0.1.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal build by default so the plugin loads in both native Apple Silicon
# and Intel builds of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64
# for a faster single-arch dev build.
#
# This must sit immediately after project() and before any target is defined.
# CMake latches the architecture list when the first target is created, and
# setting it later is silently ignored -- you get an arm64-only binary that the
# build log calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and Resolume's plugin interface
# is still OpenGL, so every SDK call warns. Silencing it is not ignoring a
# problem -- there is no alternative API to move to here.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

option(REGAUSS_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule under external/.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the plugin is, as an OBJECT library so both the bundle and the
# offline harness are built from exactly the same code.
#
# OBJECT and not STATIC on purpose. CFFGLPluginInfo registers itself from a
# file-scope constructor in Regauss.cpp and nothing ever references that object
# by name, so in an archive the linker is entitled to drop the whole translation
# unit -- and the result is a bundle that loads, exports plugMain, and reports
# that it contains no plugins.
# ---------------------------------------------------------------------------
add_library(regauss_core OBJECT
    source/Diag.h
    source/Diag.cpp
    source/Field.h
    source/Field.cpp
    source/Controls.h
    source/Controls.cpp
    source/Masks.h
    source/Masks.cpp
    source/PassBuffer.h
    source/PassBuffer.cpp
    source/Shaders.h
    source/shaders/Vertex.cpp
    source/shaders/FieldGLSL.cpp
    source/shaders/Beam.cpp
    source/shaders/Phosphor.cpp
    source/shaders/Bloom.cpp
    source/shaders/Screen.cpp
    source/Presets.h
    source/Regauss.h
    source/Regauss.cpp
)

target_include_directories(regauss_core PUBLIC source)
target_link_libraries(regauss_core PUBLIC ffgl::sdk)
target_compile_definitions(regauss_core PUBLIC REGAUSS_VERSION="${PROJECT_VERSION}")
set_target_properties(regauss_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers we include pull in <GL/glew.h>. The SDK links
# GLEW privately, so consumers need it too for the include path (and for the
# GLEW_STATIC define the static vcpkg triplet propagates). GLEW arrives through
# vcpkg.json, which nothing here names -- see the note in that file. macOS uses
# the system OpenGL framework and needs none of this.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(regauss_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin itself: a loadable macOS .bundle (MODULE library).
# ---------------------------------------------------------------------------
add_library(Regauss MODULE source/PluginEntry.cpp)
target_link_libraries(Regauss PRIVATE regauss_core)

if(APPLE)
    set_target_properties(Regauss PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        OUTPUT_NAME "Regauss"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Regauss"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ffgl.regauss"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
    # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
    target_link_libraries(Regauss PRIVATE
        "-framework OpenGL"
        "-framework Carbon"
        "-framework AppKit"
    )
endif()

# ---------------------------------------------------------------------------
# Offline harness. This machine cannot drive Resolume from a session, and "it
# compiled" is not evidence that a field model does what its comments claim.
# See tools/rgtest.
# ---------------------------------------------------------------------------
if(REGAUSS_BUILD_TOOLS AND APPLE)
    add_executable(rgtest tools/rgtest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines here
    # rather than a vendored dependency.
    target_link_libraries(rgtest PRIVATE regauss_core z
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()

# ---------------------------------------------------------------------------
# `cmake --install` drops the bundle straight into the user's Resolume plugin
# folder. Override with -DCMAKE_INSTALL_PREFIX=...
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    # honour an explicit prefix
elseif(APPLE)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()

# ---------------------------------------------------------------------------
# The OpenFX build of the same effect, for Resolve/Vegas/Nuke/Natron. CPU
# render against the vendored OFX SDK subset under external/openfx (C headers
# + the official C++ Support library, BSD-3). Field, Controls and Masks are
# linked straight from source -- so the model itself cannot drift between the
# two builds -- and only the per-pixel half is mirrored, in RegaussOFX.cpp.
# ---------------------------------------------------------------------------
option(BUILD_OFX "Build the OpenFX plugin" ON)

if(BUILD_OFX)
    set(OFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/external/openfx")
    set(OFX_BUNDLE_IDENTIFIER "com.stoatworks.ofx.regauss")

    add_library(RegaussOFX MODULE
        source/ofx/RegaussOFX.cpp
        source/Field.cpp
        source/Controls.cpp
        source/Masks.cpp
        ${OFX_SDK}/Support/Library/ofxsCore.cpp
        ${OFX_SDK}/Support/Library/ofxsImageEffect.cpp
        ${OFX_SDK}/Support/Library/ofxsInteract.cpp
        ${OFX_SDK}/Support/Library/ofxsLog.cpp
        ${OFX_SDK}/Support/Library/ofxsMultiThread.cpp
        ${OFX_SDK}/Support/Library/ofxsParams.cpp
        ${OFX_SDK}/Support/Library/ofxsProperty.cpp
        ${OFX_SDK}/Support/Library/ofxsPropertyValidation.cpp
    )
    target_include_directories(RegaussOFX PRIVATE
        source
        "${OFX_SDK}/include"
        "${OFX_SDK}/Support/include"
    )
    target_compile_definitions(RegaussOFX PRIVATE
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    )

    set(OFX_BUNDLE_DIR "${CMAKE_BINARY_DIR}/Regauss.ofx.bundle")
    set_target_properties(RegaussOFX PROPERTIES
        PREFIX ""
        SUFFIX ".ofx"
        OUTPUT_NAME "Regauss"
        LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/MacOS"
    )
    if(APPLE)
        configure_file(cmake/InfoOFX.plist.in
            "${OFX_BUNDLE_DIR}/Contents/Info.plist" @ONLY)
    elseif(WIN32)
        set_target_properties(RegaussOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            # MSVC's .exp/.lib for this target; the default directory collides
            # with the FFGL target's, which shares the output base name.
            ARCHIVE_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/importlib"
            ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/importlib")
    else()
        set_target_properties(RegaussOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Linux-x86-64")
    endif()
endif()

install(TARGETS Regauss
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)
