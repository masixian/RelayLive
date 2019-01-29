#include "stdafx.h"
#include "RtpClient.h"
#include "LiveWorker.h"

namespace HttpWsServer
{
#define PACK_MAX_SIZE 1700 

static void destroy_ring_node(void *_msg) {
    uv_buf_t *msg = (uv_buf_t*)_msg;
    free(msg->base);
    msg->base = NULL;
    msg->len = 0;
}

static void echo_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    *buf = uv_buf_init((char*)malloc(PACK_MAX_SIZE), PACK_MAX_SIZE);
}

static void after_read(uv_udp_t* handle, intptr_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags) {
    //Log::debug("after_read thread ID : %d", GetCurrentThreadId());
    if(nread < 0){
        Log::error("read error: %s",uv_strerror(nread));
        free(buf->base);
    }
    if(nread == 0)
        return;

    CRtpClient* pRtp = (CRtpClient*)handle->data;
    pRtp->RtpRecv(buf->base, nread);
}

/** rtp接收超时 */
static void timer_cb(uv_timer_t* handle) {
    CRtpClient* pRtp = (CRtpClient*)handle->data;
    pRtp->TimeOut();
}

CRtpClient::CRtpClient(CLiveWorker *live, uv_loop_t *uv, int rtpPort)
    : m_pLive(live)
    , m_uvLoop(uv)
    , m_nPort(rtpPort)
{
    m_pRtpRing  = lws_ring_create(sizeof(uv_buf_t), 1000, destroy_ring_node);
}


CRtpClient::~CRtpClient(void)
{
    Stop();
    lws_ring_destroy(m_pRtpRing);
}

void CRtpClient::StartListen()
{
    // 开启udp接收
    int ret = uv_udp_init(m_uvLoop, &m_uvRtpSocket);
    if(ret < 0) {
        Log::error("udp init error: %s", uv_strerror(ret));
        return;
    }

    struct sockaddr_in addr;
    ret = uv_ip4_addr("0.0.0.0", m_nPort, &addr);
    if(ret < 0) {
        Log::error("make address err: %s",  uv_strerror(ret));
        return ;
    }

    ret = uv_udp_bind(&m_uvRtpSocket, (struct sockaddr*)&addr, 0);
    if(ret < 0) {
        Log::error("tcp bind err: %s",  uv_strerror(ret));
        return;
    }

    int nRecvBuf = 10 * 1024 * 1024;       // 缓存区设置成10M，默认值太小会丢包
    setsockopt(m_uvRtpSocket.socket, SOL_SOCKET, SO_RCVBUF, (char*)&nRecvBuf, sizeof(nRecvBuf));
    int nOverTime = 30*1000;  //
    setsockopt(m_uvRtpSocket.socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&nOverTime, sizeof(nOverTime));
    setsockopt(m_uvRtpSocket.socket, SOL_SOCKET, SO_SNDTIMEO, (char*)&nOverTime, sizeof(nOverTime));

    m_uvRtpSocket.data = (void*)this;
    uv_udp_recv_start(&m_uvRtpSocket, echo_alloc, after_read);

    //开启udp接收超时判断
    ret = uv_timer_init(m_uvLoop, &m_uvTimeOver);
    if(ret < 0) {
        Log::error("timer init error: %s", uv_strerror(ret));
        return;
    }

    m_uvTimeOver.data = (void*)this;
    ret = uv_timer_start(&m_uvTimeOver, timer_cb,30000, 30000);
    if(ret < 0) {
        Log::error("timer start error: %s", uv_strerror(ret));
        return;
    }
}

void CRtpClient::RtpRecv(char* pBuff, long nLen)
{
    int ret = uv_timer_again(&m_uvTimeOver);
    if(ret < 0) {
        Log::error("timer again error: %s", uv_strerror(ret));
        return;
    }

    uv_buf_t newTag = uv_buf_init(pBuff, nLen);
    if (!lws_ring_insert(m_pRtpRing, &newTag, 1)) {
        destroy_ring_node(&newTag);
        Log::error("dropping!");
        return;
    }
}

void CRtpClient::TimeOut()
{
    m_pLive->stop();
}

int CRtpClient::GetPacket(char** buf, int *buf_len)
{
    uv_buf_t* e = (uv_buf_t*)lws_ring_get_element(m_pRtpRing, NULL);
    int ret = e->len;
    if(e->len > *buf_len) {
        *buf = (char*)realloc(*buf, e->len);
        *buf_len = e->len;
    }
    if(e->len >0 && e->base) {
        memcpy(*buf, e->base, e->len);
    }
    lws_ring_consume(m_pRtpRing, NULL, NULL, 1);
    return ret;
}

void CRtpClient::Stop()
{
    int ret = uv_udp_recv_stop(&m_uvRtpSocket);
    if(ret < 0) {
        Log::error("stop rtp recv port:%d err: %s", m_nPort, uv_strerror(ret));
    }
    ret = uv_timer_stop(&m_uvTimeOver);
    if(ret < 0) {
        Log::error("stop timer error: %s",uv_strerror(ret));
    }
}

}