// stdafx.cpp : 只包括标准包含文件的源文件
// stdafx.pch 将作为预编译头
// stdafx.obj 将包含预编译类型信息

#include "stdafx.h"

// TODO: 在 STDAFX.H 中
// 引用任何所需的附加头文件，而不是在此文件中引用

STREAM_TYPE g_stream_type = STREAM_UNKNOW;

#ifdef USE_FFMPEG
#pragma comment(lib,"avcodec.lib")
//#pragma comment(lib,"avdevice.lib")
//#pragma comment(lib,"avfilter.lib")
//#pragma comment(lib,"avformat.lib")
#pragma comment(lib,"avutil.lib")
#pragma comment(lib,"postproc.lib")
//#pragma comment(lib,"swresample.lib")
#pragma comment(lib,"swscale.lib")
#endif