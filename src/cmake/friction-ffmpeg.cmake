#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
# See 'README.md' for more information.
#

if(WIN32)
    # point at the isolated ffmpeg header copy, NOT sdk/include: the sdk
    # include root also carries the whole Qt5 header tree, which would
    # shadow the Qt6 headers on the compile line
    set(FFMPEG_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/sdk/ffmpeg-win)
    set(FFMPEG_LIBRARIES
        avformat
        avcodec
        avutil
        swscale
        swresample)
    set(FFMPEG_LIBRARIES_DIRS ${CMAKE_SOURCE_DIR}/sdk/bin)
else()
    # Dream Cut requires FFmpeg 7+ (AVChannelLayout / AVFrame.ch_layout API).
    # The legacy libavcodec<59 compat probe was removed: it prepended the
    # ffmpeg4.4 pkgconfig path to PKG_CONFIG_PATH and silently hijacked the
    # main libav* queries below on distros shipping both versions.
    pkg_check_modules(AVFORMAT REQUIRED libavformat)
    pkg_check_modules(AVCODEC REQUIRED libavcodec)
    pkg_check_modules(AVUTIL REQUIRED libavutil)
    pkg_check_modules(SWSCALE REQUIRED libswscale)
    pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
    set(FFMPEG_INCLUDE_DIRS ${AVFORMAT_INCLUDE_DIRS})
    set(FFMPEG_LIBRARIES
        ${AVFORMAT_LIBRARIES}
        ${AVCODEC_LIBRARIES}
        ${AVUTIL_LIBRARIES}
        ${SWSCALE_LIBRARIES}
        ${SWRESAMPLE_LIBRARIES})
    set(FFMPEG_LIBRARIES_DIRS ${AVFORMAT_LIBRARY_DIRS})
endif()
