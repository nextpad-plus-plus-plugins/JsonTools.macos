cmake_minimum_required(VERSION 3.20)
project(JsonTools VERSION 1.0.0 LANGUAGES CXX OBJCXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")

set(NPP_MACOS_DIR "${CMAKE_SOURCE_DIR}/../../notepad-plus-plus-macos")
set(SCINTILLA_INCLUDE "${NPP_MACOS_DIR}/scintilla/include")
set(NPP_PLUGIN_INCLUDE "${NPP_MACOS_DIR}/src")

set(ENGINE_SOURCES
    src/engine/JNode.cpp
    src/engine/JsonParser.cpp
    src/engine/EngineGlobals.cpp
    src/engine/RemesPathFunctions.cpp
    src/engine/RemesPathLexer.cpp
    src/engine/RemesPath.cpp
    src/engine/JsonSchemaMaker.cpp
    src/engine/JsonSchemaValidator.cpp
    src/engine/JsonTabularize.cpp
    src/engine/RandomJsonFromSchema.cpp
    src/engine/MiscModules.cpp
    src/engine/Glob.cpp
    src/engine/Regex.mm
    src/engine/JsonGrepper.mm)

add_library(JsonTools SHARED
    src/PluginEntry.mm
    src/JtNpp.mm
    src/JtSettings.mm
    src/JtCore.mm
    src/TreeViewer.mm
    src/JtForms.mm
    src/GrepperForm.mm
    ${ENGINE_SOURCES})
target_include_directories(JsonTools PRIVATE
    ${NPP_PLUGIN_INCLUDE}
    ${SCINTILLA_INCLUDE}
    src
    src/engine)
target_compile_definitions(JsonTools PRIVATE JSONTOOLS_VERSION="${PROJECT_VERSION}")
target_link_libraries(JsonTools PRIVATE "-framework Cocoa" "-framework Foundation")
target_compile_options(JsonTools PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
set_target_properties(JsonTools PROPERTIES PREFIX "" SUFFIX ".dylib")
# Stamp version into the dylib Mach-O current_version (Plugins Admin reads it).
target_link_options(JsonTools PRIVATE "-Wl,-current_version,${PROJECT_VERSION}")
set(PLUGIN_INSTALL_DIR "$ENV{HOME}/Library/Application Support/Nextpad++/plugins/JsonTools")
install(TARGETS JsonTools DESTINATION "${PLUGIN_INSTALL_DIR}")

# Engine unit tests (host-arch only, not part of the plugin). The test suite
# lives only in the local working copy (test/ is untracked), so guard the
# target or a public clone would fail to configure.
if(EXISTS "${CMAKE_SOURCE_DIR}/test/engine_test.cpp")
    add_executable(engine_test EXCLUDE_FROM_ALL
        test/engine_test.cpp
        ${ENGINE_SOURCES})
    target_include_directories(engine_test PRIVATE src/engine test)
    target_link_libraries(engine_test PRIVATE "-framework Foundation")
    target_compile_options(engine_test PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    set_target_properties(engine_test PROPERTIES OSX_ARCHITECTURES "")
endif()
