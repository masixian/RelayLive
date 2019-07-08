#pragma once

#ifdef USE_FFMPEG

extern "C"
{
#define __STDC_FORMAT_MACROS
#define snprintf  _snprintf
//#include "libavdevice/avdevice.h"
#include "libavcodec/avcodec.h"  
#include "libavformat/avformat.h"  
#include "libswscale/swscale.h"  
#include "libavutil/imgutils.h"
//#include "libavutil/timestamp.h"
}

#endif