#pragma once
namespace HttpWsServer
{
class CLiveWorker;
class CRtpClient
{
public:
    CRtpClient(CLiveWorker *live, uv_loop_t *uv, int rtpPort);
    ~CRtpClient(void);

    /** uv读取udp数据回调 */
    void RtpRecv(char* pBuff, long nLen);
    /** uv读取超时回调 */
    void TimeOut();

    /** 开始监听 */
    void StartListen();

    /** 获取一个rtp包 */
    int GetPacket(char** buf, int *buf_len);

    /** 关闭接收 */
    void Stop();
private:

private:
    uv_loop_t             *m_uvLoop;
    uv_udp_t              m_uvRtpSocket;      // rtp接收
    uv_timer_t            m_uvTimeOver;       // 接收超时定时器
    int                   m_nPort;            // rtp接收端口
    struct lws_ring       *m_pRtpRing;        // 存放rtp包
    uint32_t              m_nTail;            // 第一个数据的位置
    CLiveWorker *m_pLive;
};
}

