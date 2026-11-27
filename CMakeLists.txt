cmake_minimum_required(VERSION 3.20)
project(VolumetricCloudsExtended LANGUAGES CXX)

option(BUILD_ASI "Build the GTA SA plugin (Win32/MSVC only)" ${WIN32})
option(BUILD_TESTING "Build portable simulation tests" ON)

if(BUILD_ASI)
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "The ASI requires Visual Studio with -A Win32")
    endif()
    set(PLUGIN_SDK_DIR "$ENV{PLUGIN_SDK_DIR}" CACHE PATH "Plugin-SDK checkout")
    set(PLUGIN_SDK_LIB "${PLUGIN_SDK_DIR}/output/lib/plugin.lib" CACHE FILEPATH "Built Release Plugin-SDK library")
    if(NOT EXISTS "${PLUGIN_SDK_LIB}")
        message(FATAL_ERROR "Build Plugin-SDK Release first: ${PLUGIN_SDK_LIB}")
    endif()
    add_library(VolumetricCloudsExtended SHARED main.cpp CloudCore.hpp Cumulus.hpp)
    target_compile_features(VolumetricCloudsExtended PRIVATE cxx_std_23)
    target_compile_definitions(VolumetricCloudsExtended PRIVATE
        GTASA RW PLUGIN_SGV_10US NOMINMAX
        _CRT_SECURE_NO_WARNINGS _CRT_NON_CONFORMING_SWPRINTFS
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
        "GTAGAME_NAME=\"San Andreas\"" "GTAGAME_ABBR=\"SA\"" "GTAGAME_ABBRLOW=\"sa\""
        "GTAGAME_PROTAGONISTNAME=\"CJ\"" "GTAGAME_CITYNAME=\"San Andreas\"")
    target_include_directories(VolumetricCloudsExtended PRIVATE
        "${PLUGIN_SDK_DIR}/plugin_sa" "${PLUGIN_SDK_DIR}/plugin_sa/game_sa"
        "${PLUGIN_SDK_DIR}/plugin_sa/game_sa/rw" "${PLUGIN_SDK_DIR}/shared"
        "${PLUGIN_SDK_DIR}/shared/game" "${PLUGIN_SDK_DIR}/safetyhook")
    target_link_libraries(VolumetricCloudsExtended PRIVATE "${PLUGIN_SDK_LIB}" d3d9)
    set_target_properties(VolumetricCloudsExtended PROPERTIES
        PREFIX "" SUFFIX ".asi" MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(VolumetricCloudsExtended PRIVATE /std:c++latest /Zi)
    target_link_options(VolumetricCloudsExtended PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
endif()

if(BUILD_TESTING)
    enable_testing()
    add_executable(cloud_tests tests/cloud_tests.cpp)
    target_compile_features(cloud_tests PRIVATE cxx_std_17)
    add_test(NAME cloud_core COMMAND cloud_tests)
    add_executable(cumulus_tests tests/cumulus_tests.cpp)
    target_compile_features(cumulus_tests PRIVATE cxx_std_17)
    add_test(NAME cloud_cumulus COMMAND cumulus_tests)
endif()
