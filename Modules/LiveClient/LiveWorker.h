#pragma once
#include "LiveClient.h"
#include "avtypes.h"
#include "Recode.h"
#include "uv.h"

namespace LiveClient
{
    class CLiveReceiver;
    class CLiveChannel;
    class CLocalRtspRequest;

    class CLiveWorker : public ILiveWorker
    {
    public:
        CLiveWorker(string strCode, uint32_t rtpPort, string sdp, uint32_t ID);
        ~CLiveWorker();

        /** 客户端连接 */
        virtual bool AddHandle(ILiveHandle* h, HandleType t, int c);
        virtual bool RemoveHandle(ILiveHandle* h);
        virtual string GetSDP();

        /** 客户端全部断开，延时后销毁实例 */
        void Clear2Stop();
        bool m_bStop;          //< 进入定时器回调后设为true，close定时器回调中销毁对象
        bool m_bOver;          //< 超时后设为true，客户端全部断开后不延时，立即销毁

        /** 获取客户端信息 */
        vector<ClientInfo> GetClientInfo();

        /** 接收到的视频流处理 */
        void ReceiveStream(AV_BUFF buff);

        /** 接收数据超时发起的结束操作，通知发送连接断开 */
        void stop();

		bool play(uint32_t port);

		void parseSdp();

        bool m_bRtp;
        vector<string>           m_vecSDP;      // 缓存必要的sdp信息
		string                   m_strServerIP; // 发送端IP
		uint32_t                 m_nServerPort; //发送端口
        uint32_t                 m_nPort;       //< rtp接收端口
        string                   m_strCode;     // 播放媒体编号
        string                   m_strSDP;      // sip服务器返回的sdp
        CLocalRtspRequest       *m_pRtspReq;
        uint32_t                 m_nType;          //< 0:live直播；1:record历史视频
        ILiveHandle             *m_pHandle;
        uint32_t                 m_nID;
    };

    extern CLiveWorker* CreatLiveWorker(string strCode);
    extern CLiveWorker* GetLiveWorker(uint32_t ID);
    extern bool DelLiveWorker(uint32_t ID);
	extern string GetAllWorkerClientsInfo();
}