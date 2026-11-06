cmake_minimum_required(VERSION 3.21)
project(QuirewiseData VERSION 0.1.0 LANGUAGES C)
option(QUIREWISE_BUILD_SHARED "Build Quirewise libraries as shared libraries" OFF)
if(QUIREWISE_BUILD_SHARED)
    set(QW_LIBRARY_TYPE SHARED)
else()
    set(QW_LIBRARY_TYPE STATIC)
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(QuirewiseCharset CONFIG REQUIRED)
file(GLOB_RECURSE QW_SOURCES CONFIGURE_DEPENDS src/*.c src/*.cpp)
add_library(quirewise_data ${QW_LIBRARY_TYPE} ${QW_SOURCES})
add_library(Quirewise::Data ALIAS quirewise_data)
set_target_properties(quirewise_data PROPERTIES EXPORT_NAME Data POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_data PUBLIC c_std_11)
target_compile_definitions(quirewise_data PRIVATE QUIREWISE_STANDALONE_DATA_PARSER=1)
target_link_libraries(quirewise_data PUBLIC Quirewise::Charset)
target_include_directories(quirewise_data PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src/formats ${CMAKE_CURRENT_SOURCE_DIR}/src/backends)
install(TARGETS quirewise_data EXPORT QuirewiseDataTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT QuirewiseDataTargets FILE QuirewiseDataTargets.cmake NAMESPACE Quirewise:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseData")
configure_package_config_file(cmake/QuirewiseDataConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseDataConfig.cmake" INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseData")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/QuirewiseDataConfigVersion.cmake" VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseDataConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseDataConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseData")

if(QUIREWISE_BUILD_SHARED AND WIN32)
    set_target_properties(
        quirewise_data
        PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
