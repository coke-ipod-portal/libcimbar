cmake_minimum_required(VERSION 3.10)

project(cfc_send_chunked)

set(SOURCES
	cfc_send_chunked.cpp
)

add_executable(
	cfc_send_chunked
	${SOURCES}
)

# Links against cimbar_js (which transitively pulls in encoder, fountain,
# compression, gui/GLFW, OpenCV, wirehair, zstd, cxxopts, etc.)
target_link_libraries(cfc_send_chunked
	cimbar_js
)

add_custom_command(
	TARGET cfc_send_chunked POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:cfc_send_chunked> cfc_send_chunked.dbg
	COMMAND ${CMAKE_STRIP} -g $<TARGET_FILE:cfc_send_chunked>
)

install(
	TARGETS cfc_send_chunked
	DESTINATION bin
)
