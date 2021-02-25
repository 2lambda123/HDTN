#ifndef _TCPCL_BUNDLE_SINK_H
#define _TCPCL_BUNDLE_SINK_H 1

#include <stdint.h>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>
#include "CircularIndexBufferSingleProducerSingleConsumerConfigurable.h"
#include "Tcpcl.h"

class TcpclBundleSink {
private:
    TcpclBundleSink();
public:
    typedef boost::function<void(boost::shared_ptr<std::vector<uint8_t> > wholeBundleSharedPtr)> WholeBundleReadyCallback_t;
    //typedef boost::function<void()> ConnectionClosedCallback_t;

    TcpclBundleSink(boost::shared_ptr<boost::asio::ip::tcp::socket> tcpSocketPtr,
                    WholeBundleReadyCallback_t wholeBundleReadyCallback,
                    //ConnectionClosedCallback_t connectionClosedCallback,
                    const unsigned int numCircularBufferVectors,
                    const unsigned int circularBufferBytesPerVector);
    ~TcpclBundleSink();
    bool ReadyToBeDeleted();
private:

    void StartTcpReceive();
    void HandleTcpReceiveSome(const boost::system::error_code & error, std::size_t bytesTransferred, unsigned int writeIndex);
    void HandleTcpSend(boost::shared_ptr<std::vector<boost::uint8_t> > dataSentPtr, const boost::system::error_code& error, std::size_t bytes_transferred, bool closeSocket);
    void OnNoKeepAlivePacketReceived_TimerExpired(const boost::system::error_code& e);
    void OnNeedToSendKeepAliveMessage_TimerExpired(const boost::system::error_code& e);
    void ShutdownAndCloseTcpSocket();
    void PopCbThreadFunc();

    //tcpcl received data callback functions
    void ContactHeaderCallback(CONTACT_HEADER_FLAGS flags, uint16_t keepAliveIntervalSeconds, const std::string & localEid);
    void DataSegmentCallback(boost::shared_ptr<std::vector<uint8_t> > dataSegmentDataSharedPtr, bool isStartFlag, bool isEndFlag);
    void AckCallback(uint32_t totalBytesAcknowledged);
    void BundleRefusalCallback(BUNDLE_REFUSAL_CODES refusalCode);
    void NextBundleLengthCallback(uint32_t nextBundleLength);
    void KeepAliveCallback();
    void ShutdownCallback(bool hasReasonCode, SHUTDOWN_REASON_CODES shutdownReasonCode,
                                             bool hasReconnectionDelay, uint32_t reconnectionDelaySeconds);

    //tcpcl vars
    Tcpcl m_tcpcl;
    CONTACT_HEADER_FLAGS m_contactHeaderFlags;
    uint16_t m_keepAliveIntervalSeconds;
    std::string m_localEid;
    std::vector<uint8_t> m_fragmentedBundleRxConcat;

    WholeBundleReadyCallback_t m_wholeBundleReadyCallback;
    //ConnectionClosedCallback_t m_connectionClosedCallback;

    boost::shared_ptr<boost::asio::ip::tcp::socket> m_tcpSocketPtr;
    boost::asio::deadline_timer m_noKeepAlivePacketReceivedTimer;
    boost::asio::deadline_timer m_needToSendKeepAliveMessageTimer;

    const unsigned int M_NUM_CIRCULAR_BUFFER_VECTORS;
    const unsigned int M_CIRCULAR_BUFFER_BYTES_PER_VECTOR;
    CircularIndexBufferSingleProducerSingleConsumerConfigurable m_circularIndexBuffer;
    std::vector<std::vector<boost::uint8_t> > m_tcpReceiveBuffersCbVec;
    std::vector<std::size_t> m_tcpReceiveBytesTransferredCbVec;
    boost::condition_variable m_conditionVariableCb;
    boost::shared_ptr<boost::thread> m_threadCbReaderPtr;
    volatile bool m_running;
    volatile bool m_safeToDelete;
};



#endif  //_TCPCL_BUNDLE_SINK_H