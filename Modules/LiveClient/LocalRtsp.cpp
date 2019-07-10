#include "stdafx.h"
#include "LocalRtsp.h"
#include "LiveWorker.h"
#include "uv.h"
#include "ffmpeg.h"

namespace LiveClient
{

extern uv_loop_t *g_uv_loop;
uv_tcp_t uv_rtsp;
extern string g_strRtpIP;            //< RTSP服务IP
const int LOCAL_RTSP_PORT = 8888;    //< RTSP服务端口

/** rtsp应答内容 */
typedef struct _rtsp_response_
{
    response_code   code;
    uint32_t        CSeq;
    string          body;
    unordered_map<string,string> headers;
}rtsp_response;

static void on_close(uv_handle_t* peer) {
    CLocalRtsp* client = (CLocalRtsp*)peer->data;
    client->m_pLiveWorker->stop();
    delete client;
}

static void after_shutdown(uv_shutdown_t* req, int status) {
    uv_close((uv_handle_t*)req->handle, on_close);
    free(req);
}

static void echo_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    *buf = uv_buf_init((char*)calloc(1,1024), 1024);
}

static void after_read(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf) {
    CLocalRtsp* client = (CLocalRtsp*)handle->data;
    if (nread < 0) {
        if(nread == UV_EOF){
            Log::debug("remote close this socket");
        } else {
            Log::debug("other close %s",  uv_strerror(nread));
        }

        if (buf->base) {
            free(buf->base);
        }

        uv_shutdown_t* req = (uv_shutdown_t*) malloc(sizeof(uv_shutdown_t));
        uv_shutdown(req, handle, after_shutdown);

        return;
    }

    if (nread == 0) {
        /* Everything OK, but nothing read. */
        free(buf->base);
        return;
    }

    rtsp_handle_request(client->m_pRtspHandle, buf->base, nread);
}

static void after_write(uv_write_t* req, int status) {
    if (status < 0)
    {
        Log::error("after_write fail:%s", uv_strerror(status));
    }
}

static void on_connection(uv_stream_t* server, int status) {
    if (status != 0) {
        Log::error("Connect error %s",  uv_strerror(status));
        return;
    }

    CLocalRtsp* client = new CLocalRtsp;
    client->m_uvRtsp.data = client;

    int r = uv_tcp_init(g_uv_loop, &client->m_uvRtsp);
    if(r < 0) {
        Log::error("client init rtsp error %s",  uv_strerror(r));
        return;
    }

    r = uv_accept(server, (uv_stream_t*)&client->m_uvRtsp);
    if(r < 0) {
        Log::error("accept error %s",  uv_strerror(status));
        return;
    }

    r = uv_read_start((uv_stream_t*)&client->m_uvRtsp, echo_alloc, after_read);
    if (r < 0)
    {
        Log::error("read start error %s",  uv_strerror(r));
        return;
    }
}

void init_local_rtsp() {
    int r = uv_tcp_init(g_uv_loop, &uv_rtsp);
    if(r < 0) {
        Log::error("init local rtsp error %s",  uv_strerror(r));
        return;
    }

    struct sockaddr_in addr;
    int ret = uv_ip4_addr(g_strRtpIP.c_str(), LOCAL_RTSP_PORT, &addr);
    if(ret < 0) {
        Log::error("make address err: %s",  uv_strerror(ret));
        return;
    }

    ret = uv_tcp_bind(&uv_rtsp, (struct sockaddr*)&addr, 0);
    if(ret < 0) {
        Log::error("tcp bind err: %s",  uv_strerror(ret));
        return;
    }

    ret = uv_listen((uv_stream_t*)&uv_rtsp, SOMAXCONN, on_connection);
    if (ret < 0)
    {
        Log::error("uv listen err:%s", uv_strerror(ret));
        return;
    }

    Log::debug("rtsp server[127.0.0.1:8888] init success");
}

static void request_cb(void *user, rtsp_ruquest_t *req) {
    CLocalRtsp* client = (CLocalRtsp*)user;
    client->answer(req);
}

static string make_session_id(){
    static uint64_t sid = 10000001;
    return StringHandle::toStr<uint64_t>(sid++);
}

CLocalRtsp::CLocalRtsp(void)
{
    m_pRtspHandle = create_rtsp(this, request_cb);
}

CLocalRtsp::~CLocalRtsp(void)
{
	destory_rtsp(m_pRtspHandle);
}

int CLocalRtsp::answer(rtsp_ruquest_t *req)
{
    rtsp_response res;

    do{
        if(req->method == rtsp_method::RTSP_ERROR) {
            res.code = Code_400_BadRequest;
            break;
        }

        res.CSeq = req->CSeq;   //应答序号和请求序号一致

        if(req->method == rtsp_method::RTSP_OPTIONS) {
            res.code = Code_200_OK;
            res.headers.insert(make_pair("Public","OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE"));
        } else if(req->method == rtsp_method::RTSP_DESCRIBE) {
            uint32_t ID;
            sscanf(req->uri, "rtsp://%*[^/]/%d", &ID);
            m_pLiveWorker = GetLiveWorker(ID);
            if(!m_pLiveWorker) {
                res.code = Code_400_BadRequest;
                break;
            }
			//m_strServerIP = m_pLiveWorker->m_strServerIP;
			//m_nServerPort = m_pLiveWorker->m_nServerPort;
			//m_nClientPort = m_pLiveWorker->m_nPort;

   //         stringstream ss;
   //         ss << "v=0\r\n";
   //         for(auto it:m_pLiveWorker->m_vecSDP){
   //             ss << it << "\r\n";
   //         }
   //         ss << "a=recvonly\r\n";

            res.code = Code_200_OK;
            //res.body = ss.str();
			res.body = "v=0\r\n"
						"o=36030100062000000000 0 0 IN IP4 10.9.0.2\r\n"
						"s=Play\r\n"
						"u=36030100061320000026:3\r\n"
						"c=IN IP4 127.0.0.1\r\n"
						"t=0 0\r\n"
						"a=sdplang:en\r\n"
						"a=range:npt=0-\r\n"
						"a=control:*\r\n"
						"m=video 18000 RTP/AVP 96\r\n"
						"a=rtpmap:96 MP2P/90000\r\n"
						"a=recvonly\r\n"
						"y=0301000168\r\n";
            res.headers.insert(make_pair("Content-Type","application/sdp"));
            res.headers.insert(make_pair("Content-Length",StringHandle::toStr<size_t>(res.body.size())));
        } else if(req->method == rtsp_method::RTSP_SETUP) {
			uint32_t ID;
            sscanf(req->uri, "rtsp://%*[^/]/%d", &ID);
			m_pLiveWorker = GetLiveWorker(ID);
			if(!m_pLiveWorker || !m_pLiveWorker->play(req->rtp_port)) {
                res.code = Code_400_BadRequest;
                break;
            }
			m_strServerIP = m_pLiveWorker->m_strServerIP;
			m_nServerPort = m_pLiveWorker->m_nServerPort;
			m_nClientPort = m_pLiveWorker->m_nPort;

			char transport[50]={0};
			sprintf(transport, "RTP/AVP;unicast;client_port=%d-%d;source=%s;server_port=%d-%d", m_nClientPort, m_nClientPort+1, m_strServerIP.c_str(),m_nServerPort, m_nServerPort+1);
            res.code = Code_200_OK;
			res.headers.insert(make_pair("Transport",transport));
            res.headers.insert(make_pair("Session",make_session_id()));
        } else if(req->method == rtsp_method::RTSP_PLAY) {
            res.code = Code_200_OK;
            res.headers.insert(make_pair("Session",rtsp_request_get_header(req, "Session")));
        } else if(req->method == rtsp_method::RTSP_PAUSE) {
            res.code = Code_200_OK;
            res.headers.insert(make_pair("Session",rtsp_request_get_header(req, "Session")));
        } else if(req->method == rtsp_method::RTSP_TEARDOWN) {
            res.code = Code_200_OK;
            res.headers.insert(make_pair("Session",rtsp_request_get_header(req, "Session")));
        } else {
            res.code = Code_551_OptionNotSupported;
        }
    }while(0);

    //生成tcp应答报文
    char time_buff[50]={0};
    time_t time_now = time(NULL);
    ctime_s(time_buff, 49, &time_now);
    string strTime = time_buff;
    strTime = strTime.substr(0,strTime.size()-1);
    strTime += " GMT";
    stringstream ss;
    ss << "RTSP/1.0 " << response_status[res.code] << "\r\n"
        << "CSeq: " << res.CSeq << "\r\n"
        << "Date: " << strTime << "\r\n";
    for (auto& h:res.headers) {
        ss << h.first << ": " << h.second << "\r\n";
    }
    if (!res.body.empty()) {
        ss << "Content-Length: " << res.body.size()+2 << "\r\n\r\n";
        ss << res.body;
    }
    ss << "\r\n";
    string strResponse = ss.str();
	Log::debug(strResponse.c_str());

    //发送应答
    uv_write_t *wr = (uv_write_t*)malloc(sizeof(uv_write_t));
    wr->data = this;
    uv_buf_t buff = uv_buf_init((char*)strResponse.c_str(), strResponse.size());
    int ret = uv_write(wr, (uv_stream_t*)&m_uvRtsp,&buff, 1, after_write);
    if (ret < 0) {
        Log::error("uv_write fail:%s",  uv_strerror(ret));
    }
    return 0;
}


//////////////////////////////////////////////////////////////////////////

static int write_buffer(void *opaque, uint8_t *buf, int buf_size){
    CLiveWorker* pLive = (CLiveWorker*)opaque;
    //Log::debug("write_buffer size:%d \n", buf_size);
    AV_BUFF tmp = {AV_TYPE::FLV_FRAG_KEY, (char*)buf, buf_size, 0, 0};
    pLive->ReceiveStream(tmp);
    return buf_size;
}

static void ffmpeg_play_thread(void* arg) {
    CLocalRtspRequest* rtsp = (CLocalRtspRequest*)arg;
	rtsp->Play();
}

CLocalRtspRequest::CLocalRtspRequest(uint32_t ID, uint32_t port)
    : m_nWorkerID(ID)
    , m_nRtpPort(port)
{
	uv_thread_t tid;
    //uv_thread_create(&tid, ffmpeg_play_thread, this);
}

CLocalRtspRequest::~CLocalRtspRequest()
{

}

bool CLocalRtspRequest::Play()
{
    AVFormatContext *ifc = NULL;
    AVFormatContext *ofc = NULL;

    //输入 打开rtsp
    char rtsp_uri[MAX_PATH]={0};
    sprintf(rtsp_uri, "rtsp://%s:%d/%d",g_strRtpIP.c_str(), LOCAL_RTSP_PORT, m_nWorkerID);

	Log::debug("%s   %d", rtsp_uri, m_nRtpPort);

    AVDictionary* options = NULL;
    av_dict_set(&options, "buffer_size", "102400", 0); //设置缓存大小，1080p可将值调大
    av_dict_set(&options, "rtsp_transport", "udp", 0); //以udp方式打开，如果以tcp方式打开将udp替换为tcp
    //av_dict_set(&options, "stimeout", "2000000", 0); //设置超时断开连接时间，单位微秒
    //av_dict_set(&options, "max_delay", "500000", 0); //设置最大时延
    char rtp_port[10]={0};
    sprintf(rtp_port, "%d", m_nRtpPort);
	char rtcp_port[10]={0};
    sprintf(rtcp_port, "%d", m_nRtpPort+2);
    av_dict_set(&options, "min_port", rtp_port, 0);
    av_dict_set(&options, "max_port", rtcp_port, 0);
    int ret = avformat_open_input(&ifc, rtsp_uri, NULL, &options);
    if (ret != 0) {
        char tmp[1024]={0};
        av_strerror(ret, tmp, 1024);
        Log::error("Could not open input file '%s': %d(%s)", rtsp_uri, ret, tmp);
        goto end;
    }
    ret = avformat_find_stream_info(ifc, NULL);
    if (ret < 0) {
        char tmp[1024]={0};
        av_strerror(ret, tmp, 1024);
        Log::error("Failed to retrieve input stream information %d(%s)", ret, tmp);
        goto end;
    }
    Log::debug("show input format info");
    av_dump_format(ifc, 0, g_strRtpIP.c_str(), 0);

    //输出 自定义回调
    AVOutputFormat *ofmt = NULL;
    ret = avformat_alloc_output_context2(&ofc, NULL, "flv", NULL);
    if (!ofc) {
        Log::error("Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }
    ofmt = ofc->oformat;

    unsigned char* outbuffer = (unsigned char*)av_malloc(65536);
    AVIOContext *avio_out = avio_alloc_context(outbuffer, 65536, 1, m_pLiveWorker, NULL, write_buffer, NULL);  
    ofc->pb = avio_out; 
    ofc->flags = AVFMT_FLAG_CUSTOM_IO;
    ofmt->flags |= AVFMT_NOFILE;

    //根据输入流信息生成输出流信息
    int in_video_index = -1, in_audio_index = -1, in_subtitle_index = -1;
    int out_video_index = -1, out_audio_index = -1, out_subtitle_index = -1;
    for (unsigned int i = 0, j = 0; i < ifc->nb_streams; i++) {
        AVStream *is = ifc->streams[i];
        if (is->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
            in_video_index = i;
            out_video_index = j++;
            //} else if (is->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            //    in_audio_index = i;
            //    out_audio_index = j++;
            //} else if (is->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            //    in_subtitle_index = i;
            //    out_subtitle_index = j++;
        } else {
            continue;
        }

        AVStream *os = avformat_new_stream(ofc, NULL);
        if (!os) {
            Log::error("Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        ret = avcodec_parameters_copy(os->codecpar, is->codecpar);
        if (ret < 0) {
            Log::error("Failed to copy codec parameters\n");
            goto end;
        }
    }
    Log::debug("show output format info");
    av_dump_format(ofc, 0, NULL, 1);

    //Log::debug("index:%d %d %d %d",in_video_index, in_audio_index, out_video_index, out_audio_index);

    ret = avformat_write_header(ofc, NULL);
    if (ret < 0) {
        char tmp[1024]={0};
        av_strerror(ret, tmp, 1024);
        Log::error("Error avformat_write_header %d:%s \n", ret, tmp);
        goto end;
    }

    bool first = true;
    m_bStop = false;
    while (!m_bStop) {
        AVStream *in_stream, *out_stream;
        AVPacket pkt;
        av_init_packet(&pkt);
        ret = av_read_frame(ifc, &pkt);
        if (ret < 0)
            break;
        //Log::debug("read_index %d",pkt.stream_index);
        in_stream  = ifc->streams[pkt.stream_index];
        if (pkt.stream_index == in_video_index) {
            //Log::debug("video dts %d", pkt.dts);
            //Log::debug("video pts %d", pkt.pts);
            pkt.stream_index = out_video_index;
            out_stream = ofc->streams[pkt.stream_index];
            /* copy packet */
            if(first){
                pkt.pts = 0;
                pkt.dts = 0;
                first = false;
            } else {
                pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF/*|AV_ROUND_PASS_MINMAX*/);
                pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF/*|AV_ROUND_PASS_MINMAX*/);
            }
            pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
            pkt.pos = -1;
            //Log::debug("video2 dts %d", pkt.dts);
            //Log::debug("video2 pts %d", pkt.pts);

            int wret = av_interleaved_write_frame(ofc, &pkt);
            if (wret < 0) {
                char tmp[1024]={0};
                av_strerror(ret, tmp, 1024);
                Log::error("video error muxing packet %d:%s \n", ret, tmp);
                //break;
            }
        } //else if (pkt.stream_index == in_audio_index) {
        //    //Log::debug("audio dts %d pts %d", pkt.dts, pkt.pts);
        //    pkt.stream_index = out_audio_index;
        //    out_stream = ofc->streams[pkt.stream_index];
        //    /* copy packet */
        //    pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF/*|AV_ROUND_PASS_MINMAX*/);
        //    pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF/*|AV_ROUND_PASS_MINMAX*/);
        //    pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
        //    pkt.pos = -1;
        //    //Log::debug("audio2 dts %d pts %d", pkt.dts, pkt.pts);

        //    int wret = av_interleaved_write_frame(ofc, &pkt);
        //    if (wret < 0) {
        //        char tmp[1024]={0};
        //        av_strerror(ret, tmp, 1024);
        //        Log::error("audio error muxing packet %d:%s \n", ret, tmp);
        //        //break;
        //    }
        //}
        av_packet_unref(&pkt);
    }

    av_write_trailer(ofc);
end:
    /** 关闭输入 */
    avformat_close_input(&ifc);

    /* 关闭输出 */
    if (ofc && !(ofmt->flags & AVFMT_NOFILE))
        avio_closep(&ofc->pb);
    avformat_free_context(ofc);

    /** 返回码 */
    if (ret < 0 /*&& ret != AVERROR_EOF*/) {
        char tmp[AV_ERROR_MAX_STRING_SIZE]={0};
        av_make_error_string(tmp,AV_ERROR_MAX_STRING_SIZE,ret);
        Log::error("Error occurred: %s\n", tmp);
        return false;
    }
    Log::debug("client stop, delete live worker");
    delete this;
    return true;
}

}