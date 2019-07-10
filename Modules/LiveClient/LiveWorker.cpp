#include "stdafx.h"
#include "uv.h"
#include "LiveWorker.h"
#include "LiveClient.h"
#include "liveReceiver.h"
#include "LiveChannel.h"
#include "LiveIpc.h"
#include "bnf.h"
#include "LocalRtsp.h"

namespace LiveClient
{
	extern uv_loop_t *g_uv_loop;

    extern string g_strRtpIP;            //< RTP����IP
    extern int    g_nRtpBeginPort;       //< RTP��������ʼ�˿ڣ�������ż��
    extern int    g_nRtpPortNum;         //< RTPʹ�õĸ�������strRTPPort��ʼÿ�μ�2����strRTPNum��
    extern int    g_nRtpCatchPacketNum;  //< rtp����İ�������
	extern int    g_nRtpStreamType;      //< rtp�������ͣ�����libLive��ps h264

    extern vector<int>     m_vecRtpPort;     //< RTP���ö˿ڣ�ʹ��ʱ����ȡ����ʹ�ý������·���
    //extern CriticalSection m_csRTP;          //< RTP�˿���

    static map<int,CLiveWorker*>  m_workerMap;
    //static CriticalSection           m_cs;

    static int GetRtpPort()
    {
        //MutexLock lock(&m_csRTP);

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
        //MutexLock lock(&m_csRTP);
        m_vecRtpPort.push_back(nPort);
    }


    /** ��ʱ���ٶ�ʱ����loop���Ƴ���� */
    static void stop_timer_close_cb(uv_handle_t* handle) {
        CLiveWorker* live = (CLiveWorker*)handle->data;
        if (live->m_bStop){
            live->Clear2Stop();
        } else {
            Log::debug("new client comed, and will not close live stream");
        }
    }

    /** �ͻ���ȫ���Ͽ�����ʱ�Ͽ�Դ�Ķ�ʱ�� */
	static void stop_timer_cb(uv_timer_t* handle) {
		CLiveWorker* live = (CLiveWorker*)handle->data;
		int ret = uv_timer_stop(handle);
		if(ret < 0) {
			Log::error("timer stop error:%s",uv_strerror(ret));
        }
        live->m_bStop = true;
        uv_close((uv_handle_t*)handle, stop_timer_close_cb);
	}

    /** CLiveWorker������ɾ��m_pLive�ȽϺ�ʱ��������event loop�����ʹ���̡߳� */
    static void live_worker_destory_thread(void* arg) {
        CLiveWorker* live = (CLiveWorker*)arg;
        SAFE_DELETE(live);
    }

    //////////////////////////////////////////////////////////////////////////

    CLiveWorker* CreatLiveWorker(string strCode)
    {
        Log::debug("CreatFlvBuffer begin");
        int rtpPort = GetRtpPort();
        if(rtpPort < 0) {
            Log::error("play failed %s, no rtp port",strCode.c_str());
            return nullptr;
        }

        string sdp;
        //if(LiveIpc::RealPlay(strCode, g_strRtpIP,  rtpPort, sdp))
        //{
        //    //uv_thread_t tid;
        //    //uv_thread_create(&tid, live_worker_destory_thread, pNew);
        //    Log::error("play failed %s",strCode.c_str());
        //    return nullptr;
        //}
		//
        //Log::debug("RealPlay ok: %s",strCode.c_str());

        static int ID = 0;
        ID++;
        CLiveWorker* pNew = new CLiveWorker(strCode, rtpPort, sdp, ID);

        //MutexLock lock(&m_cs);
        m_workerMap.insert(make_pair(ID, pNew));

        return pNew;
    }

    CLiveWorker* GetLiveWorker(uint32_t ID)
    {
        auto itFind = m_workerMap.find(ID);
        if (itFind != m_workerMap.end())
        {
            // �Ѿ�����
            return itFind->second;
        }
        return nullptr;
    }

    bool DelLiveWorker(uint32_t ID)
    {
        auto itFind = m_workerMap.find(ID);
        if (itFind != m_workerMap.end())
        {
            // CLiveWorker������ɾ��m_pLive�ȽϺ�ʱ��������event loop�����ʹ���߳����ٶ���
            uv_thread_t tid;
            uv_thread_create(&tid, live_worker_destory_thread, itFind->second);

            m_workerMap.erase(itFind);
            return true;
        }
        return false;
    }

	string GetAllWorkerClientsInfo(){
        stringstream ss;
		ss << "{\"root\":[";
        //MutexLock lock(&m_cs);
        for (auto w : m_workerMap) {
            CLiveWorker *worker = w.second;
			vector<ClientInfo> tmp = worker->GetClientInfo();
            for(auto c:tmp){
                ss << "{\"DeviceID\":\"" << c.devCode 
                    << "\",\"Connect\":\"" << c.connect
                    << "\",\"Media\":\"" << c.media
                    << "\",\"ClientIP\":\"" << c.clientIP
                    << "\",\"Channel\":\"" << c.channel
                    << "\"},";
            }
		}
        string strResJson = StringHandle::StringTrimRight(ss.str(),',');
        strResJson += "]}";
        return strResJson;
	}

    //////////////////////////////////////////////////////////////////////////

    static void H264DecodeCb(AV_BUFF buff, void* user) {
        CLiveWorker* live = (CLiveWorker*)user;
        //live->ReceiveYUV(buff);
    }

    CLiveWorker::CLiveWorker(string strCode, uint32_t rtpPort, string sdp, uint32_t ID)
        : m_strCode(strCode)
        , m_nPort(rtpPort)
        , m_strSDP(sdp)
        , m_nID(ID)
        , m_nType(0)
        , m_bStop(false)
        , m_bOver(false)
    {
        parseSdp();

        m_pRtspReq = new CLocalRtspRequest(ID, rtpPort);
    }

    CLiveWorker::~CLiveWorker()
    {
        string ssid = StringHandle::toStr<int>(m_nPort);
        if(LiveIpc::StopPlay(ssid)) {
            Log::error("stop play failed");
        }

        GiveBackRtpPort(m_nPort);
        Log::debug("CLiveWorker release");
    }

    bool CLiveWorker::AddHandle(ILiveHandle* h, HandleType t, int c)
    {
        m_pHandle = h;
        return true;
    }

    bool CLiveWorker::RemoveHandle(ILiveHandle* h)
    {
        // ԭʼͨ��
        Clear2Stop();
        return true;
    }

    string CLiveWorker::GetSDP(){
        return m_strSDP;
    }

	void CLiveWorker::Clear2Stop() {
        DelLiveWorker(m_nID);
	}

    void CLiveWorker::stop()
    {
        //��ƵԴû�����ݲ���ʱ
        Log::debug("no data recived any more, stopped");
        //״̬�ı�Ϊ��ʱ����ʱǰ��ȫ���Ͽ�������Ҫ��ʱ��ֱ������
        m_bOver = true;
        m_pHandle->stop();
    }

    vector<ClientInfo> CLiveWorker::GetClientInfo()
    {
		vector<ClientInfo> ret;
        ClientInfo c = m_pHandle->get_clients_info();
        ret.push_back(c);
		return ret;
    }

    void CLiveWorker::ReceiveStream(AV_BUFF buff)
    {
        m_pHandle->push_video_stream(buff);
    }

	bool CLiveWorker::play(uint32_t port)
	{
		 m_nPort = port;
        if(LiveIpc::RealPlay(m_strCode, g_strRtpIP,  port, m_strSDP))
        {
            //uv_thread_t tid;
            //uv_thread_create(&tid, live_worker_destory_thread, pNew);
            Log::error("play failed %s",m_strCode.c_str());
            return false;
        }
		parseSdp();
		
        Log::debug("RealPlay ok: %s",m_strCode.c_str());
		return true;
	}

	void CLiveWorker::parseSdp()
	{
		//��sdp��������ƵԴip�Ͷ˿�
        bnf_t* sdp_bnf = create_bnf(m_strSDP.c_str(), m_strSDP.size());
        char *sdp_line = NULL;
        char remoteIP[25]={0};
        int remotePort = 0;
        while (bnf_line(sdp_bnf, &sdp_line)) {
            if(sdp_line[0]=='c'){
                sscanf(sdp_line, "c=IN IP4 %[^/\r\n]", remoteIP);
				m_strServerIP = remoteIP;
            } else if(sdp_line[0]=='m') {
                sscanf(sdp_line, "m=video %d ", &remotePort);
				m_nServerPort = remotePort;
            }

            if(sdp_line[0]=='o' || sdp_line[0]=='s' || sdp_line[0]=='c' || sdp_line[0]=='t' || sdp_line[0]=='m'){
                char tmp[256]={0};
                sscanf(sdp_line, "%[^\r\n]", tmp);
                m_vecSDP.push_back(tmp);
            } else if(sdp_line[0]=='a' && !strncmp(sdp_line, "a=rtpmap:", 9)){
				//a=rtpmap:96 PS/90000
                char tmp[256]={0};
				int num = 0;
				char type[20]={0};
				int bitrate = 0;
                sscanf(sdp_line, "a=rtpmap:%d %[^/]/%d", &num, type, &bitrate);
				if(!strcmp(type, "PS")){
					sprintf(tmp,"a=rtpmap:%d MP2P/%d", num, bitrate);
				}else{
					sprintf(tmp,"a=rtpmap:%d %s/%d", num, type, bitrate);
				}
                m_vecSDP.push_back(tmp);
            }
        }
		destory_bnf(sdp_bnf);
	}
}
