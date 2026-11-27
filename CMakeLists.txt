cmake_minimum_required(VERSION 3.20)
project(VolumetricCloudsExtended LANGUAGES CXX)

option(BUILD_ASI "Build the GTA SA plugin (Win32/MSVC only)" ${WIN32})
option(BUILD_TESTING "Build portable simulation tests" ON)

# Compile the optional hardware vertex path using the Windows SDK. No
# D3DX/D3DCompiler DLL or runtime compilation is required on the player's PC.
if(WIN32 AND (BUILD_ASI OR BUILD_TESTING))
    find_program(CLOUD_FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64")
    if(NOT CLOUD_FXC)
        file(GLOB fxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe")
        list(SORT fxc_candidates COMPARE NATURAL ORDER DESCENDING)
        if(fxc_candidates)
            list(GET fxc_candidates 0 CLOUD_FXC)
        endif()
    endif()
    if(CLOUD_FXC)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/CloudBankVS.hpp"
            COMMAND "${CLOUD_FXC}" /nologo /T vs_2_0 /E main /O3 /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/CloudBankVS.hpp" /Vn cloudBankVS "${CMAKE_CURRENT_SOURCE_DIR}/CloudBankVS.hlsl"
            DEPENDS CloudBankVS.hlsl VERBATIM)
        add_custom_target(cloud_bank_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated/CloudBankVS.hpp")
    else()
        message(WARNING "fxc not found; using the portable CPU cloud vertex backend")
    endif()
endif()
function(enable_cloud_vertex_shader target)
    if(TARGET cloud_bank_shader)
        add_dependencies(${target} cloud_bank_shader)
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_compile_definitions(${target} PRIVATE VC_GPU_VERTEX=1)
    endif()
endfunction()

if(BUILD_ASI)
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "The ASI requires Visual Studio with -A Win32")
    endif()
    set(PLUGIN_SDK_DIR "$ENV{PLUGIN_SDK_DIR}" CACHE PATH "Plugin-SDK checkout")
    set(PLUGIN_SDK_LIB "${PLUGIN_SDK_DIR}/output/lib/plugin.lib" CACHE FILEPATH "Built Release Plugin-SDK library")
    if(NOT EXISTS "${PLUGIN_SDK_LIB}")
        message(FATAL_ERROR "Build Plugin-SDK Release first: ${PLUGIN_SDK_LIB}")
    endif()
    add_library(VolumetricCloudsExtended SHARED main.cpp CloudCore.hpp CloudWeather.hpp Canopy.hpp CloudBanks.hpp CloudBillboards.hpp CloudD3D9.hpp)
    enable_cloud_vertex_shader(VolumetricCloudsExtended)
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
    add_executable(canopy_tests tests/canopy_tests.cpp)
    target_compile_features(canopy_tests PRIVATE cxx_std_17)
    # RenderWare/Windows headers expose legacy near/far macros. Emulate
    # those names in portable tests to catch collisions before the ASI build.
    target_compile_definitions(canopy_tests PRIVATE near= far=)
    add_test(NAME cloud_canopy COMMAND canopy_tests)
    add_executable(weather_tests tests/weather_tests.cpp)
    target_compile_features(weather_tests PRIVATE cxx_std_17)
    target_compile_definitions(weather_tests PRIVATE near= far=)
    add_test(NAME cloud_weather COMMAND weather_tests)
    add_executable(banks_tests tests/banks_tests.cpp)
    target_compile_features(banks_tests PRIVATE cxx_std_17)
    target_compile_definitions(banks_tests PRIVATE near= far=)
    add_test(NAME cloud_banks COMMAND banks_tests)
    add_executable(cloud_bake_cache tools/bake_cache.cpp)
    target_compile_features(cloud_bake_cache PRIVATE cxx_std_17)
    add_executable(cache_tests tests/cache_tests.cpp)
    target_compile_features(cache_tests PRIVATE cxx_std_17)
    add_test(NAME cloud_cache COMMAND cache_tests)
    if(WIN32)
        add_executable(d3d9_volume_test tests/d3d9_volume_test.cpp)
        enable_cloud_vertex_shader(d3d9_volume_test)
        target_compile_features(d3d9_volume_test PRIVATE cxx_std_17)
        target_link_libraries(d3d9_volume_test PRIVATE d3d9 user32)
        add_test(NAME cloud_d3d9_volume COMMAND d3d9_volume_test)
        set_tests_properties(cloud_d3d9_volume PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()
