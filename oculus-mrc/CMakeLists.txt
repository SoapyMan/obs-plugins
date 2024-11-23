cmake_minimum_required(VERSION 3.28...3.30)

add_library(oculus-mrc MODULE)
add_library(OBS::oculus-mrc ALIAS oculus-mrc)

target_sources(oculus-mrc PRIVATE
	oculus-mrc.cpp
	log.h
	frame.h
	frame.cpp
)
target_link_libraries(
  oculus-mrc
  PRIVATE
    OBS::libobs
    FFmpeg::avcodec
    FFmpeg::avfilter
    FFmpeg::avformat
    FFmpeg::avdevice
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
    $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>
)

set_target_properties_obs(oculus-mrc PROPERTIES FOLDER plugins/oculus-mrc PREFIX "")
