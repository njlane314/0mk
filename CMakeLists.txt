cmake_minimum_required(VERSION 3.16)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/0mk.h" MK0_VERSION_LINE
     REGEX "^inline constexpr std::string_view version =")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" MK0_VERSION "${MK0_VERSION_LINE}")
if(NOT MK0_VERSION)
    message(FATAL_ERROR "Could not read the 0mk version from 0mk.h")
endif()
project(
    0mk
    VERSION "${MK0_VERSION}"
    DESCRIPTION "Make-shaped content-addressed pipeline runner"
    LANGUAGES CXX
)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(mk0_header INTERFACE)
add_library(0mk::0mk ALIAS mk0_header)
set_target_properties(mk0_header PROPERTIES EXPORT_NAME 0mk)
target_compile_features(mk0_header INTERFACE cxx_std_17)
target_include_directories(
    mk0_header
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

add_executable(0mk 0mk.cpp)
set_target_properties(
    0mk
    PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        EXPORT_NAME cli
)
target_link_libraries(0mk PRIVATE 0mk::0mk)

if(BUILD_TESTING)
    add_test(NAME 0mk.command-tests COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh")
    set_tests_properties(0mk.command-tests PROPERTIES ENVIRONMENT "CXX=${CMAKE_CXX_COMPILER}")
    add_test(NAME 0mk.v02-tests
             COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/v02.sh" $<TARGET_FILE:0mk>)
endif()

install(TARGETS 0mk EXPORT 0mkTargets RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(TARGETS mk0_header EXPORT 0mkTargets INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES 0mk.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES README.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(DIRECTORY docs/ DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs")

configure_package_config_file(
    cmake/0mkConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/0mkConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/0mk"
)
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(MK0_VERSION_COMPATIBILITY SameMinorVersion)
else()
    set(MK0_VERSION_COMPATIBILITY SameMajorVersion)
endif()
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/0mkConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY "${MK0_VERSION_COMPATIBILITY}"
)
install(
    EXPORT 0mkTargets
    FILE 0mkTargets.cmake
    NAMESPACE 0mk::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/0mk"
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/0mkConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/0mkConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/0mk"
)

set(CPACK_PACKAGE_NAME 0mk)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if(NOT CPACK_SYSTEM_NAME)
    set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(CPACK_PACKAGE_FILE_NAME "0mk-${PROJECT_VERSION}-${CPACK_SYSTEM_NAME}")
set(CPACK_GENERATOR TGZ)
include(CPack)
