#pragma once

class CRtpClient;
namespace HttpWsServer
{
    struct pss_http_ws_live;
    enum MediaType;

    struct LIVE_BUFF {
        char *pBuff;
        int   nLen;
    };

    class CLiveWorker
    {
    public:
        CLiveWorker(string strCode, int rtpPort, pss_http_ws_live *pss);
        ~CLiveWorker();
        /** 通知播放进程播放 */
        bool RealPlayAsync(int rtpPort);
        /** 播放成功回调 */
        void RealPlaySuccess();
        /** 通知播放进程关闭 */
        void StopAsync();
        /** ffmpeg解编码线程 */
        bool Play();
        /** ffmpeg读取输入 */
        int ReadInput(uint8_t *buf, int buf_size);

        /** 启动UDP端口监听 */
        void StartListen();

		/** 客户端全部断开，延时后销毁实例 */
		void Clear2Stop();
        bool m_bStop;          //< 进入定时器回调后设为true，close定时器回调中销毁对象
        bool m_bOver;          //< 超时后设为true，客户端全部断开后不延时，立即销毁

        /** 请求端获取视频数据 */
        LIVE_BUFF GetFlvVideo(uint32_t *tail);
        void NextWork(pss_http_ws_live* pss);

        /** 获取客户端信息 */
        string GetClientInfo();

        /**
         * 从源过来的视频数据，单线程输入 
         * 以下继承自IlibLiveCb的方法由rtp接收所在的loop线程调用
         * 类中其他方法包括构造、析构都由http所在的loop线程调用
         */
        void push_flv_frame(char* pBuff, int nLen);
        void stop();
    private:
        void cull_lagging_clients(MediaType type);


    private:
        string                m_strCode;     // 播放媒体编号
        CRtpClient            *m_pRtp;
        char                  *m_pRtpBuff;
        int                   m_nRtpBuffLen;
        int                   m_nRtpLen;
        int                   m_nRtpRead;

        /**
         * lws_ring无锁环形缓冲区，只能一个线程写入，一个线程读取
         */
        struct lws_ring       *m_pRing;
        pss_http_ws_live      *m_pPssList;


        int                   m_nType;          //< 0:live直播；1:record历史视频
        int                   m_nPort;          //< rtp接收端口

        uv_timer_t            m_uvTimerPlayTimeOut; //< 播放超时没有得到响应
    };

    /** ipc 初始化 */
    void ipc_init();

    /** 直播 */
    CLiveWorker* CreatLiveWorker(string strCode, pss_http_ws_live *pss);

    /** 点播 */

    /** 获取播放信息，返回json */
    string GetClientsInfo();
};