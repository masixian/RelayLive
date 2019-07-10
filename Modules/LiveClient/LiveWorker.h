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

        /** �ͻ������� */
        virtual bool AddHandle(ILiveHandle* h, HandleType t, int c);
        virtual bool RemoveHandle(ILiveHandle* h);
        virtual string GetSDP();

        /** �ͻ���ȫ���Ͽ�����ʱ������ʵ�� */
        void Clear2Stop();
        bool m_bStop;          //< ���붨ʱ���ص�����Ϊtrue��close��ʱ���ص������ٶ���
        bool m_bOver;          //< ��ʱ����Ϊtrue���ͻ���ȫ���Ͽ�����ʱ����������

        /** ��ȡ�ͻ�����Ϣ */
        vector<ClientInfo> GetClientInfo();

        /** ���յ�����Ƶ������ */
        void ReceiveStream(AV_BUFF buff);

        /** �������ݳ�ʱ����Ľ���������֪ͨ�������ӶϿ� */
        void stop();

		bool play(uint32_t port);

		void parseSdp();

        bool m_bRtp;
        vector<string>           m_vecSDP;      // �����Ҫ��sdp��Ϣ
		string                   m_strServerIP; // ���Ͷ�IP
		uint32_t                 m_nServerPort; //���Ͷ˿�
        uint32_t                 m_nPort;       //< rtp���ն˿�
        string                   m_strCode;     // ����ý����
        string                   m_strSDP;      // sip���������ص�sdp
        CLocalRtspRequest       *m_pRtspReq;
        uint32_t                 m_nType;          //< 0:liveֱ����1:record��ʷ��Ƶ
        ILiveHandle             *m_pHandle;
        uint32_t                 m_nID;
    };

    extern CLiveWorker* CreatLiveWorker(string strCode);
    extern CLiveWorker* GetLiveWorker(uint32_t ID);
    extern bool DelLiveWorker(uint32_t ID);
	extern string GetAllWorkerClientsInfo();
}