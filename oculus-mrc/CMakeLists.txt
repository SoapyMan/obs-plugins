project(oculus-mrc)

option(ENABLE_OCULUS_MRC "Enable Oculus MRC plugin" ON)

if(NOT ENABLE_OCULUS_MRC)
  target_disable(oculus-mrc)
  return()
endif()

if(MSVC)
	set(oculus-mrc_PLATFORM_DEPS w32-pthreads)
endif()

find_package(FFmpeg REQUIRED COMPONENTS avcodec avfilter avdevice avutil swscale avformat swresample)
include_directories(${FFMPEG_INCLUDE_DIRS})

add_library(oculus-mrc MODULE)
add_library(OBS::oculus-mrc ALIAS oculus-mrc)

target_sources(oculus-mrc PRIVATE
	oculus-mrc.cpp
	log.h
	frame.h
	frame.cpp
)
target_link_libraries(oculus-mrc PRIVATE OBS::libobs ${oculus-mrc_PLATFORM_DEPS} ${FFMPEG_LIBRARIES})

set_target_properties(oculus-mrc PROPERTIES FOLDER "plugins")

setup_plugin_target(oculus-mrc)