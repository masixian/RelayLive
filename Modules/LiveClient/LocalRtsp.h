#pragma once
#include "netstream.h"
#include "rtsp.h"
#include "uv.h"

namespace LiveClient
{

extern void init_local_rtsp();

class CLiveWorker;

class CLocalRtsp
{
public:
    CLocalRtsp(void);
    ~CLocalRtsp(void);
    int answer(rtsp_ruquest_t *req);
    
    uv_tcp_t            m_uvRtsp;         //rtsp���Ӿ��
    rtsp               *m_pRtspHandle;
    string              m_strDevCode;     //�豸ID
    string              m_strServerIP;
    string              m_strClientIP;
    int                 m_nServerPort;
    int                 m_nClientPort;

    CLiveWorker        *m_pLiveWorker;
};

/**
 * ʹ��ffmepg����rtsp������rtp����
 */
class CLocalRtspRequest
{
public:
    CLocalRtspRequest(uint32_t ID, uint32_t port);
    ~CLocalRtspRequest();

    bool Play();
    uint32_t      m_nWorkerID;  //liveworker��ID
    uint32_t      m_nRtpPort;   // ����rtp�˿�
    bool          m_bStop;

    CLiveWorker  *m_pLiveWorker;
};

}