#include "stdafx.h"
#include "RtpClient.h"
#include "HttpLiveServer.h"
#include "LiveWorker.h"
//其他模块
#include "uvIpc.h"

#define PACK_MAX_SIZE 1700 
#define FFMPEG_INPUT_SIZE 32768

extern "C"
{
#define snprintf  _snprintf
#include "libavdevice/avdevice.h"
#include "libavcodec/avcodec.h"  
#include "libavformat/avformat.h"  
#include "libswscale/swscale.h"  
#include "libavutil/imgutils.h"
#include "libavutil/timestamp.h"
}
#pragma comment(lib,"avcodec.lib")
#pragma comment(lib,"avdevice.lib")
#pragma comment(lib,"avfilter.lib")
#pragma comment(lib,"avformat.lib")
#pragma comment(lib,"avutil.lib")
#pragma comment(lib,"postproc.lib")
#pragma comment(lib,"swresample.lib")
#pragma comment(lib,"swscale.lib")

namespace HttpWsServer
{
    //////////////////////////////////////////////////////////////////////////////////

	extern uv_loop_t *g_uv_loop;
    static uv_ipc_handle_t* h = NULL;
    static map<int,CLiveWorker*>  m_workerMap;
    static CriticalSection           m_cs;

    static string m_strRtpIP;            //< RTP服务IP
    static int    m_nRtpBeginPort;       //< RTP监听的起始端口，必须是偶数
    static int    m_nRtpPortNum;         //< RTP使用的个数，从strRTPPort开始每次加2，共strRTPNum个
    static int    m_nRtpCatchPacketNum;  //< rtp缓存的包的数量
	static int    m_nRtpStreamType;      //< rtp包的类型，传给libLive。ps h264

    static vector<int>     m_vecRtpPort;     //< RTP可用端口，使用时从中取出，使用结束重新放入
    static CriticalSection m_csRTP;          //< RTP端口锁

    ////////////////////////////////////////////////////////////////////////////////////////////////

    static string strfind(char* src, char* begin, char* end){
        char *p1, *p2;
        p1 = strstr(src, begin);
        if(!p1) return "";
        p1 += strlen(begin);
        p2 = strstr(p1, end);
        if(p2) return string(p1, p2-p1);
        else return string(p1);
    }

    static void on_ipc_recv(uv_ipc_handle_t* h, void* user, char* name, char* msg, char* data, int len) {
        if (!strcmp(msg,"live_play_answer")) {
            // ssid=123&ret=0&error=XXXX
            data[len] = 0;
            int ssid = stoi(strfind(data, "ssid=", "&"));
            int ret = stoi(strfind(data, "ret=", "&"));
            string error = strfind(data, "error=", "&");

            MutexLock lock(&m_cs);
            auto itFind = m_workerMap.find(ssid);
            if (itFind != m_workerMap.end())
            {
                if (!ret) {
                    itFind->second->RealPlaySuccess();
                } else {
                    itFind->second->RealPlayFailed();
                }
            }
        }
    }

    void ipc_init(){
        /** 进程间通信 */
        int ret = uv_ipc_client(&h, "relay_live", NULL, "liveDest", on_ipc_recv, NULL);
        if(ret < 0) {
            printf("ipc server err: %s\n", uv_ipc_strerr(ret));
        }
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////

    static int GetRtpPort()
    {
        static bool _do_port = false;
        MutexLock lock(&m_csRTP);
        if(!_do_port) {
            _do_port = true;
            m_strRtpIP           = Settings::getValue("RtpClient","IP");                    //< RTP服务IP
            m_nRtpBeginPort      = Settings::getValue("RtpClient","BeginPort",10000);       //< RTP监听的起始端口，必须是偶数
            m_nRtpPortNum        = Settings::getValue("RtpClient","PortNum",1000);          //< RTP使用的个数，从strRTPPort开始每次加2，共strRTPNum个
            m_nRtpCatchPacketNum = Settings::getValue("RtpClient", "CatchPacketNum", 100);  //< rtp缓存的包的数量
			m_nRtpStreamType     = Settings::getValue("RtpClient", "Filter", 0);            //< rtp缓存的包的数量

            Log::debug("RtpConfig IP:%s, BeginPort:%d,PortNum:%d,CatchPacketNum:%d"
                , m_strRtpIP.c_str(), m_nRtpBeginPort, m_nRtpPortNum, m_nRtpCatchPacketNum);
            m_vecRtpPort.clear();
            for (int i=0; i<m_nRtpPortNum; ++i) {
                m_vecRtpPort.push_back(m_nRtpBeginPort+i*2);
            }
        }

        int nRet = -1;
        auto it = m_vecRtpPort.begin();
        if (it != m_vecRtpPort.end()) {
            nRet = *it;
            m_vecRtpPort.erase(it);
        }

        return nRet;
    }

    static void GiveBackRtpPort(int nPort)
    {
        MutexLock lock(&m_csRTP);
        m_vecRtpPort.push_back(nPort);
    }

    static void destroy_ring_node(void *_msg)
    {
        LIVE_BUFF *msg = (LIVE_BUFF*)_msg;
        free(msg->pBuff);
        msg->pBuff = NULL;
        msg->nLen = 0;
    }


    /** CLiveWorker析构中删除m_pLive比较耗时，会阻塞event loop，因此使用线程。 */
    static void live_worker_destory_thread(void* arg) {
        CLiveWorker* live = (CLiveWorker*)arg;
        SAFE_DELETE(live);
    }

    /** 播放超时，没有收到播放进程返回的结果 */
    static void play_timeout_timer_cb(uv_timer_t* handle){
        CLiveWorker *pLive = (CLiveWorker*)handle->data;
        pLive->stop();
    }

    /** ffmpeg解码编码线程 */
    static void real_play(void* arg){
        CLiveWorker* pLive = (CLiveWorker*)arg;
        pLive->Play();
    }

    static int read_buffer(void *opaque, uint8_t *buf, int buf_size){
        CLiveWorker* pLive = (CLiveWorker*)opaque;
        return pLive->ReadInput(buf, buf_size);
    }

    static int write_buffer(void *opaque, uint8_t *buf, int buf_size){
        CLiveWorker* pLive = (CLiveWorker*)opaque;
        //Log::debug("write_buffer size:%d \n", buf_size);
        pLive->push_flv_frame((char*)buf, buf_size);
        return buf_size;
    }

    //////////////////////////////////////////////////////////////////////////

    CLiveWorker::CLiveWorker(string strCode, int rtpPort, pss_http_ws_live *pss)
        : m_strCode(strCode)
        , m_nPort(rtpPort)
        , m_pPssList(pss)
        , m_nType(0)
        , m_bStop(false)
        , m_bOver(false)
        , m_nRtpLen(0)
        , m_nRtpRead(0)
    {
        m_pRing  = lws_ring_create(sizeof(LIVE_BUFF), 100, destroy_ring_node);
        m_pRtp = new CRtpClient(this, g_uv_loop, rtpPort);
        m_pRtpBuff = (char*)malloc(FFMPEG_INPUT_SIZE);
        m_nRtpBuffLen = FFMPEG_INPUT_SIZE;
    }

    CLiveWorker::~CLiveWorker()
    {
        StopAsync();
        lws_ring_destroy(m_pRing);
        GiveBackRtpPort(m_nPort);
        Log::debug("CLiveWorker release");
    }

    bool CLiveWorker::RealPlayAsync(int rtpPort)
    {
        // ssid=123&rtpip=1.1.1.1&rtpport=50000
        stringstream ss;
        ss << "ssid=" << m_strCode << "&rtpip=" << m_strRtpIP << "&rtpport=" << rtpPort;
        int ret = uv_ipc_send(h, "liveSrc", "live_play", (char*)ss.str().c_str(), ss.str().size());
        if(ret) {
            Log::error("ipc send real play error");
            return false;
        }

        m_uvTimerPlayTimeOut.data = this;
        uv_timer_init(g_uv_loop, &m_uvTimerPlayTimeOut);
        uv_timer_start(&m_uvTimerPlayTimeOut, play_timeout_timer_cb, 20000, 0);
        
        return true;
    }

    void CLiveWorker::RealPlaySuccess()
    {
        //成功将定时器关掉即可
        if(uv_is_active((const uv_handle_t*)&m_uvTimerPlayTimeOut)) {
            uv_timer_stop(&m_uvTimerPlayTimeOut);
            uv_close((uv_handle_t*)&m_uvTimerPlayTimeOut, NULL);
        }
		m_pRtp->StartListen();
        uv_thread_t tid;
        uv_thread_create(&tid, real_play, (void*)this);
    }

	void CLiveWorker::RealPlayFailed()
	{
		//成功将定时器关掉即可
        if(uv_is_active((const uv_handle_t*)&m_uvTimerPlayTimeOut)) {
            uv_timer_stop(&m_uvTimerPlayTimeOut);
            uv_close((uv_handle_t*)&m_uvTimerPlayTimeOut, NULL);
        }
		stop();
	}

    void CLiveWorker::StopAsync()
    {
        string ssid = StringHandle::toStr<int>(m_nPort);
        int ret = uv_ipc_send(h, "liveSrc", "stop_play", (char*)ssid.c_str(), ssid.size());
        if(ret) {
            Log::error("ipc send stop error");
        }
    }

    bool CLiveWorker::Play()
    {
        AVFormatContext *ifc = NULL;
        AVInputFormat *ifmt = NULL;
        AVFormatContext *ofc = NULL;

        //流读取
        unsigned char * iobuffer=(unsigned char *)av_malloc(FFMPEG_INPUT_SIZE);
        AVIOContext *pb = avio_alloc_context(iobuffer, FFMPEG_INPUT_SIZE, 0, this, read_buffer, NULL, NULL);
        //探测流格式
        int ret = av_probe_input_buffer(pb, &ifmt, "", NULL, 0, 0);
        if (ret != 0) {
            char tmp[1024]={0};
            av_strerror(ret, tmp, 1024);
            Log::error("av_probe_input_buffer failed: %d(%s)", ret, tmp);
            goto end;
        }
        Log::debug("av_probe_input_buffer  %s[%s]", ifmt->name, ifmt->long_name);
        //打开流
        ifc = avformat_alloc_context();
        ifc->pb = pb;
        ret = avformat_open_input(&ifc, "", ifmt, NULL);
        if (ret != 0) {
            char tmp[1024]={0};
            av_strerror(ret, tmp, 1024);
            Log::error("Could not open input file: %d(%s)", ret, tmp);
            goto end;
        }
        //解析流信息
        ret = avformat_find_stream_info(ifc, NULL);
        if (ret < 0) {
            char tmp[1024]={0};
            av_strerror(ret, tmp, 1024);
            Log::error("Failed to retrieve input stream information %d(%s)", ret, tmp);
            goto end;
        }
        Log::debug("show input format info");
        av_dump_format(ifc, 0, NULL, 0);

        //输出 自定义回调
        AVOutputFormat *ofmt = NULL;
        ret = avformat_alloc_output_context2(&ofc, NULL, "flv", NULL);
        if (!ofc) {
            Log::error("Could not create output context\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
        ofmt = ofc->oformat;

        unsigned char* outbuffer=(unsigned char*)av_malloc(65536);
        AVIOContext *avio_out =avio_alloc_context(outbuffer, 65536,1,this,NULL,write_buffer,NULL);  
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
            stop();
            return false;
        }
        Log::debug("client stop, delete live worker");
        delete this;
        return true;
    }

    int CLiveWorker::ReadInput(uint8_t *buf, int buf_size)
    {
        if(m_nRtpRead >= m_nRtpLen){
            m_nRtpLen = m_pRtp->GetPacket(&m_pRtpBuff, &m_nRtpBuffLen);
            m_nRtpRead = 0;
            if(m_nRtpLen == 0)
                return 0;
        }
        int nDataSize = m_nRtpLen - m_nRtpRead;
        if(nDataSize > buf_size){
            memcpy(buf, m_pRtpBuff+m_nRtpRead, buf_size);
            m_nRtpRead += buf_size;
            return buf_size;
        } else {
            memcpy(buf, m_pRtpBuff+m_nRtpRead, nDataSize);
            m_nRtpRead += nDataSize;
            return nDataSize;
        }
    }

    void CLiveWorker::StartListen()
    {
        m_pRtp->StartListen();
    }

	void CLiveWorker::Clear2Stop() {
        Log::debug("need close live stream");
        if(m_bStop) {
            Log::debug("already stop, delete live worker");
            delete this;
        } else {
            Log::debug("need stop ffmpeg");
            m_bStop = true;
        }
	}

    void CLiveWorker::push_flv_frame(char* pBuff, int nLen)
    {
        //内存数据保存至ring-buff
        int n = (int)lws_ring_get_count_free_elements(m_pRing);
        Log::debug("LWS_CALLBACK_RECEIVE: free space %d\n", n);
        if (!n)
            return;

        // 将数据保存在ring buff
        char* pSaveBuff = (char*)malloc(nLen + LWS_PRE);
        memcpy(pSaveBuff + LWS_PRE, pBuff, nLen);
        LIVE_BUFF newTag = {pSaveBuff, nLen};
        if (!lws_ring_insert(m_pRing, &newTag, 1)) {
            destroy_ring_node(&newTag);
            Log::error("dropping!");
            return;
        }

        //向客户端发送数据
        lws_callback_on_writable(m_pPssList->wsi);
    }

    void CLiveWorker::stop()
    {
        //视频源没有数据并超时
        Log::debug("no data recived any more, stopped");
        //状态改变为超时，此时前端全部断开，不需要延时，直接销毁

        //断开客户端连接
        lws_set_timeout(m_pPssList->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
    }

    LIVE_BUFF CLiveWorker::GetFlvVideo(uint32_t *tail)
    {
        LIVE_BUFF ret = {nullptr,0};
        LIVE_BUFF* tag = (LIVE_BUFF*)lws_ring_get_element(m_pRing, tail);
        if(tag) ret = *tag;

        return ret;
    }

    void CLiveWorker::NextWork(pss_http_ws_live* pss)
    {
        struct lws_ring *ring = m_pRing;
        pss_http_ws_live* pssList = m_pPssList;

        //Log::debug("this work tail:%d\r\n", pss->tail);
        lws_ring_consume_and_update_oldest_tail(
            ring,	          /* lws_ring object */
            pss_http_ws_live, /* type of objects with tails */
            &pss->tail,	      /* tail of guy doing the consuming */
            1,		          /* number of payload objects being consumed */
            pssList,	      /* head of list of objects with tails */
            tail,		      /* member name of tail in objects with tails */
            pss_next	      /* member name of next object in objects with tails */
            );
        //Log::debug("next work tail:%d\r\n", pss->tail);

        /* more to do for us? */
        if (lws_ring_get_element(ring, &pss->tail))
            /* come back as soon as we can write more */
                lws_callback_on_writable(pss->wsi);
    }

    string CLiveWorker::GetClientInfo()
    {
        stringstream ss;
        ss << "{\"DeviceID\":\"" << m_strCode << "\",\"Connect\":\"";
        if(m_pPssList->isWs){
            ss << "web socket";
        } else {
            ss << "Http";
        }
        ss << "\",\"Media\":\"";
        if(m_pPssList->media_type == media_flv)
            ss << "flv";
        else if(m_pPssList->media_type == media_mp4)
            ss << "mp4";
        else if(m_pPssList->media_type == media_h264)
            ss << "h264";
        ss << "\",\"ClientIP\":\"" 
           << m_pPssList->clientIP << "\"},";

        return ss.str();
    }

    void CLiveWorker::cull_lagging_clients(MediaType type)
    {
        struct lws_ring *ring = m_pRing;
        pss_http_ws_live* pssList = m_pPssList;
  

        uint32_t oldest_tail = lws_ring_get_oldest_tail(ring);
        pss_http_ws_live *old_pss = NULL;
        int most = 0, before = lws_ring_get_count_waiting_elements(ring, &oldest_tail), m;

        lws_start_foreach_llp_safe(pss_http_ws_live **, ppss, pssList, pss_next) {
            if ((*ppss)->tail == oldest_tail) {
                //连接超时
                old_pss = *ppss;
                Log::debug("Killing lagging client %p", (*ppss)->wsi);
                lws_set_timeout((*ppss)->wsi, PENDING_TIMEOUT_LAGGING, LWS_TO_KILL_ASYNC);
                (*ppss)->culled = 1;
                lws_ll_fwd_remove(pss_http_ws_live, pss_next, (*ppss), pssList);
                continue;
            } else {
                m = lws_ring_get_count_waiting_elements(ring, &((*ppss)->tail));
                if (m > most)
                    most = m;
            }
        } lws_end_foreach_llp_safe(ppss);

        if (!old_pss)
            return;

        lws_ring_consume_and_update_oldest_tail(ring,
            pss_http_ws_live, &old_pss->tail, before - most,
            pssList, tail, pss_next);

        Log::debug("shrunk ring from %d to %d", before, most);
    }

    //////////////////////////////////////////////////////////////////////////

    CLiveWorker* CreatLiveWorker(string strCode, pss_http_ws_live *pss)
    {
        Log::debug("CreatLiveWorker begin");
        int rtpPort = GetRtpPort();
        if(rtpPort < 0) {
            Log::error("play failed %s, no rtp port",strCode.c_str());
            return nullptr;
        }

        CLiveWorker* pNew = new CLiveWorker(strCode, rtpPort, pss);
        {
            MutexLock lock(&m_cs);
            m_workerMap.insert(make_pair(rtpPort, pNew));
        }
        //if(!SipInstance::RealPlay(strCode, m_strRtpIP,  rtpPort))
        if(!pNew->RealPlayAsync(rtpPort))
        {
            pNew->stop();
            Log::error("play failed %s",strCode.c_str());
            return nullptr;
        }
        Log::debug("RealPlay ok: %s",strCode.c_str());

        return pNew;
    }

    string GetClientsInfo() 
    {
        MutexLock lock(&m_cs);
        auto it = m_workerMap.begin();
        auto end = m_workerMap.end();
        string strResJson = "{\"root\":[";
        for (;it != end; ++it)
        {
            strResJson += it->second->GetClientInfo();
        }
        strResJson = StringHandle::StringTrimRight(strResJson,',');
        strResJson += "]}";
        return strResJson;
    }
}