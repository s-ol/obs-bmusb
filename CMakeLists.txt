cmake_minimum_required(VERSION 3.16)
project(obs-bmusb VERSION 0.1.0)

find_package(libobs REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(BMUSB REQUIRED IMPORTED_TARGET bmusb)

add_library(${PROJECT_NAME} MODULE bmusb-source.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs PkgConfig::BMUSB)

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins)

