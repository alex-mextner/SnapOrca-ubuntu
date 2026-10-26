# slic3rutils tests require libslic3r_gui (contains Http.cpp).
# Skip entirely when GUI is disabled.
if (NOT SLIC3R_GUI)
    return()
endif()

get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    SnapLogClient_test.cpp
    PartPlateCli_test.cpp
    )

find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(libnoise REQUIRED)
find_package(Qhull 7.2 QUIET)
if(UNIX AND NOT APPLE)
    target_link_options(${_TEST_NAME}_tests PRIVATE "-Wl,--no-as-needed")
endif()
if(TARGET libslic3r_gui)
    # libslic3r_gui compiles the nanoSVG implementation itself (BitmapCache.cpp); linking test_nanosvg_impl as well
    # defines every nsvg* symbol twice.
    set(_gui_test_link_libs ${TEST_LINK_LIBS})
    list(REMOVE_ITEM _gui_test_link_libs test_nanosvg_impl)
    target_link_libraries(${_TEST_NAME}_tests PRIVATE libslic3r_gui ${_gui_test_link_libs})
    # libslic3r_gui calls into sources that only the application executable compiles (Sentry wrapper, bury point,
    # common_func); compile them into the test executable too.
    target_sources(${_TEST_NAME}_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp
        ${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp
        ${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp)
    target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if (SLIC3R_SENTRY)
        target_compile_definitions(${_TEST_NAME}_tests PRIVATE SLIC3R_SENTRY)
        target_link_libraries(${_TEST_NAME}_tests PRIVATE sentry::sentry)
    endif ()
else()
    target_link_libraries(${_TEST_NAME}_tests PRIVATE ${TEST_LINK_LIBS})
endif()
if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests PRIVATE Setupapi.lib)
endif ()
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        Snapmaker_Orca_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
    elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
        Snapmaker_Orca_copy_dlls(COPY_DLLS "RelWithDebInfo" "" output_dlls_Release)
    else()
        Snapmaker_Orca_copy_dlls(COPY_DLLS "Release" "" output_dlls_Release)
    endif()
endif()

catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: " EXTRA_ARGS ${CATCH_EXTRA_ARGS})
# add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests "${CATCH_EXTRA_ARGS} exclude:[NotWorking]")
