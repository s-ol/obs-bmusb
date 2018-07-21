project(obs-bmusb)

set(obs-bmusb_SOURCES
	bmusb-source.cpp)

find_package(PkgConfig REQUIRED)
pkg_check_modules(BMUSB REQUIRED bmusb)

add_library(obs-bmusb MODULE
	${obs-bmusb_SOURCES})
target_include_directories(obs-bmusb PUBLIC
	${BMUSB_INCLUDE_DIRS})
target_link_libraries(obs-bmusb
	libobs
	bmusb
	${BMUSB_LIBRARIES})

install_obs_plugin_with_data(obs-bmusb data)
