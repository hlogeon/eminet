//
//  EmiP2P.h
//  eminet
//
//  Created by Per Eckerdal on 2012-04-23.
//  Copyright (c) 2012 Per Eckerdal. All rights reserved.
//

#ifndef eminet_EmiP2PSock_h
#define eminet_EmiP2PSock_h

#include "EmiTypes.h"
#include "EmiNetUtil.h"
#include "EmiP2PSockConfig.h"
#include "EmiP2PConn.h"
#include "EmiMessageHeader.h"
#include "EmiMessage.h"
#include "EmiAddressCmp.h"
#include "EmiUdpSocket.h"
#include "EmiPacketHeader.h"
#include "EmiNetRandom.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

static const EmiTimeInterval EMI_P2P_COOKIE_RESOLUTION  = 5*60; // In seconds

template<class Binding>
class EmiP2PSock {
public:
    
    
    static const size_t          EMI_P2P_SERVER_SECRET_SIZE = 32;
    static const size_t          EMI_P2P_SHARED_SECRET_SIZE = 32;
    static const size_t          EMI_P2P_RAND_NUM_SIZE = 8;
    static const size_t          EMI_P2P_COOKIE_SIZE = EMI_P2P_RAND_NUM_SIZE + Binding::HMAC_HASH_SIZE;
    
protected:
    typedef typename Binding::TemporaryData TemporaryData;
    typedef typename Binding::Error         Error;
    typedef typename Binding::TimerCookie   TimerCookie;
    
    typedef EmiP2PConn<Binding, EmiP2PSock, EMI_P2P_RAND_NUM_SIZE> Conn;
    typedef EmiUdpSocket<Binding> EUS;
    
    typedef EmiP2PSockConfig                                 SockConfig;
    typedef typename Conn::ConnCookieRandNum                 ConnCookieRandNum;
    typedef std::map<sockaddr_storage, Conn*, EmiAddressCmp> ConnMap;
    typedef typename ConnMap::iterator                       ConnMapIter;
    typedef std::map<ConnCookieRandNum, Conn*>               ConnCookieMap;
    typedef typename ConnCookieMap::iterator                 ConnCookieMapIter;
    
private:
    // Private copy constructor and assignment operator
    inline EmiP2PSock(const EmiP2PSock& other);
    inline EmiP2PSock& operator=(const EmiP2PSock& other);
    
    TimerCookie _timerCookie;
    
    sockaddr_storage _address;
    
    uint8_t  _serverSecret[EMI_P2P_SERVER_SECRET_SIZE];
    EUS     *_socket;
    
    // The keys of this map are the Conn*'s peer addresses;
    // each conn has two entries in _conns.
    ConnMap       _conns;
    ConnCookieMap _connCookies;
    
    inline bool shouldArtificiallyDropPacket() const {
        if (0 == config.fabricatedPacketDropRate) return false;
        
        return EmiNetRandom<Binding>::randomFloat() < config.fabricatedPacketDropRate;
    }
    
    void hashCookie(EmiTimeInterval stamp, const uint8_t *randNum,
                    uint8_t *buf, size_t bufLen,
                    bool complementary,
                    bool minusOne = false) const {
        ASSERT(bufLen >= Binding::HMAC_HASH_SIZE);
        
        uint8_t complementaryByte = (complementary ? 0 : 1);
        
        uint64_t integerStamp = static_cast<uint64_t>(floor(stamp/EMI_P2P_COOKIE_RESOLUTION)) - (minusOne ? 1 : 0);
        
        uint8_t toBeHashed[EMI_P2P_RAND_NUM_SIZE+sizeof(integerStamp)+sizeof(complementaryByte)];
        
        memcpy(toBeHashed, randNum, EMI_P2P_RAND_NUM_SIZE);
        *((uint64_t *)(toBeHashed+EMI_P2P_RAND_NUM_SIZE)) = integerStamp;
        toBeHashed[EMI_P2P_RAND_NUM_SIZE+sizeof(integerStamp)] = complementaryByte;
        
        Binding::hmacHash(_serverSecret, sizeof(_serverSecret),
                          toBeHashed, sizeof(toBeHashed),
                          buf, bufLen);
    }
    
    // Returns true if the cookie is valid
    bool checkCookie(EmiTimeInterval stamp,
                     bool complementary,
                     const uint8_t *buf, size_t bufLen) const {
        if (EMI_P2P_COOKIE_SIZE != bufLen) {
            return false;
        }
        
        uint8_t testBuf[Binding::HMAC_HASH_SIZE];
        
        hashCookie(stamp, buf, testBuf, sizeof(testBuf), complementary);
        if (0 == memcmp(testBuf, buf+EMI_P2P_RAND_NUM_SIZE, sizeof(testBuf))) {
            return true;
        }
        
        hashCookie(stamp, buf, testBuf, sizeof(testBuf), complementary, /*minusOne:*/true);
        if (0 == memcmp(testBuf, buf+EMI_P2P_RAND_NUM_SIZE, sizeof(testBuf))) {
            return true;
        }
        
        return false;
    }
    
    // Returns true if the cookie is valid. If the cookie was valid,
    // sets the wasComplementary parameter.
    bool checkCookie(EmiTimeInterval stamp,
                     const uint8_t *cookie, size_t cookieLen,
                     bool *wasComplementary) const {
        if (EMI_P2P_COOKIE_SIZE != cookieLen) {
            return false;
        }
        
        if (checkCookie(stamp, /*complementary:*/false, cookie, cookieLen)) {
            *wasComplementary = false;
            return true;
        }
        
        if (checkCookie(stamp, /*complementary:*/true, cookie, cookieLen)) {
            *wasComplementary = true;
            return true;
        }
        
        return false;
    }
    
    Conn *findConn(const sockaddr_storage& address) {
        ConnMapIter cur = _conns.find(address);
        return _conns.end() == cur ? NULL : (*cur).second;
    }
    
    void gotConnectionOpen(EmiTimeInterval now,
                           EmiSequenceNumber initialSequenceNumber,
                           const uint8_t *rawData,
                           size_t len,
                           EUS *sock,
                           const sockaddr_storage& inboundAddress,
                           const sockaddr_storage& remoteAddress,
                           const uint8_t *cookie,
                           size_t cookieLength) {
        bool cookieIsComplementary;
        if (!checkCookie(now, cookie, cookieLength, &cookieIsComplementary)) {
            // Invalid cookie. Ignore packet.
            return;
        }
        
        Conn *conn = findConn(remoteAddress);
        
        if (conn && conn->isInitialSequenceNumberMismatch(remoteAddress, initialSequenceNumber)) {
            // The connection is already open, and we get a SYN message with a
            // different initial sequence number. This probably means that the
            // other host has forgot about the connection we have open. Drop it
            // and continue as if conn did not exist.
            removeConnection(conn);
            conn = NULL;
        }
        
        if (!conn) {
            // We did not already have a connection with this address
            // Check to see if we have a connection with this cookie
            ConnCookieRandNum cc(cookie, cookieLength);
            
            ConnCookieMapIter cur = _connCookies.find(cc);
            
            if (_connCookies.end() != cur) {
                // There was a connection open with this cookie
                
                conn = (*cur).second;
                
                if (conn->firstPeerHadComplementaryCookie() == cookieIsComplementary) {
                    // This happens if we get a SYN message with the same cookie data
                    // from more than one remote address. This ought not to happen,
                    // unfortunately it does, because at first, EmiConn will tell
                    // EmiUdpSocket to send packets from every bound interface, which
                    // might make EmiP2PSock receive multiple packets from different
                    // addresses even though they are sent from the same peer.
                    //
                    // This packet should be ignored.
                    return;
                }
                
                // We don't need to save the cookie anymore
                _connCookies.erase(cur);
                
                conn->gotOtherAddress(inboundAddress, remoteAddress, initialSequenceNumber);
            }
            else {
                // There was no connection open with this cookie. Open a new one
                
                conn = new Conn(*this, _timerCookie,
                                initialSequenceNumber,
                                cc, cookieIsComplementary,
                                sock, remoteAddress,
                                config.connectionTimeout,
                                config.initialConnectionTimeout,
                                config.rateLimit);
                _connCookies.insert(std::make_pair(cc, conn));
            }
            
            _conns.insert(std::make_pair(remoteAddress, conn));
        }
        
        // Regardless of whether we had an EmiP2PConn object set up
        // for this address, we want to reply to the host with an
        // acknowledgement that we have received the SYN message.
        conn->sendPrx(inboundAddress, remoteAddress);
    }
    
    // conn must not be NULL
    void gotConnectionOpenAck(const sockaddr_storage& inboundAddress,
                              const sockaddr_storage& remoteAddress,
                              Conn *conn,
                              EmiTimeInterval now,
                              const uint8_t *rawData,
                              size_t len) {
        const size_t ipLen = EmiNetUtil::ipLength(remoteAddress);
        static const size_t portLen = 2;
        if (len != ipLen+portLen) {
            // Invalid packet
            return;
        }
        
        sockaddr_storage innerAddress;
        
        EmiNetUtil::makeAddress(remoteAddress.ss_family,
                                rawData, ipLen,
                                *((uint16_t *)(rawData+ipLen)),
                                &innerAddress);
        
        conn->gotInnerAddress(remoteAddress, innerAddress);
        
        if (conn->hasBothInnerAddresses()) {
            conn->sendEndpointPair(inboundAddress, 0);
            conn->sendEndpointPair(inboundAddress, 1);
        }
    }
    
    static void onMessage(EUS *socket,
                          void *userData,
                          EmiTimeInterval now,
                          const sockaddr_storage& inboundAddress,
                          const sockaddr_storage& remoteAddress,
                          const TemporaryData& data,
                          size_t offset,
                          size_t len) {
        EmiP2PSock *sock((EmiP2PSock *)userData);
        sock->onMessage(now, socket, inboundAddress, remoteAddress, data, offset, len);
    }
    
public:
    
    // This method is public because it is also invoked by EmiP2PConn
    //
    // conn might be NULL. In that case, this is a no-op
    void removeConnection(Conn *conn) {
        if (conn) {
            _conns.erase(conn->getFirstAddress());
            _conns.erase(conn->getOtherAddress());
            _connCookies.erase(conn->cookie);
            
            delete conn;
        }
    }
    
    const SockConfig config;
    
    EmiP2PSock(const SockConfig& config_, const TimerCookie& timerCookie) :
    _timerCookie(timerCookie), _socket(NULL), config(config_) {
        Binding::randomBytes(_serverSecret, sizeof(_serverSecret));
    }
    virtual ~EmiP2PSock() {
        if (_socket) {
            delete _socket;
            _socket = NULL;
        }
        
        ConnMapIter iter = _conns.begin();
        ConnMapIter end  = _conns.end();
        while (iter != end) {
            delete (*iter).second;
            ++iter;
        }
    }
    
    bool isOpen() const {
        return !!_socket;
    }
    
    template<class SocketCookie>
    bool open(const SocketCookie& socketCookie, Error& err) {
        if (!_socket) {
            memcpy(&_address, &config.address, sizeof(_address));
            EmiNetUtil::addrSetPort(_address, config.port);
            _socket = EUS::open(socketCookie,
                                onMessage,
                                this,
                                _address,
                                err);
            if (!_socket) return false;
        }
        
        return true;
    }
    
    // Returns the size of the cookie
    size_t generateCookiePair(EmiTimeInterval stamp,
                              uint8_t *bufA, size_t bufALen,
                              uint8_t *bufB, size_t bufBLen) const {
        ASSERT(bufALen >= EMI_P2P_COOKIE_SIZE);
        ASSERT(bufBLen >= EMI_P2P_COOKIE_SIZE);
        
        Binding::randomBytes(bufA, EMI_P2P_RAND_NUM_SIZE);
        memcpy(bufB, bufA, EMI_P2P_RAND_NUM_SIZE);
        
        hashCookie(stamp, /*randNum:*/bufA,
                   bufA+EMI_P2P_RAND_NUM_SIZE, bufALen-EMI_P2P_RAND_NUM_SIZE,
                   /*complementary:*/false);
        
        hashCookie(stamp, /*randNum:*/bufB,
                   bufB+EMI_P2P_RAND_NUM_SIZE, bufBLen-EMI_P2P_RAND_NUM_SIZE,
                   /*complementary:*/true);
        
        return EMI_P2P_COOKIE_SIZE;
    }
    
    // Returns the size of the shared secret
    static size_t generateSharedSecret(uint8_t *buf, size_t bufLen) {
        ASSERT(bufLen >= EMI_P2P_SHARED_SECRET_SIZE);
        
        Binding::randomBytes(buf, EMI_P2P_SHARED_SECRET_SIZE);
        
        return EMI_P2P_SHARED_SECRET_SIZE;
    }
    
    void onMessage(EmiTimeInterval now,
                   EUS *sock,
                   const sockaddr_storage& inboundAddress,
                   const sockaddr_storage& remoteAddress,
                   const TemporaryData& data,
                   size_t offset,
                   size_t len) {
        if (shouldArtificiallyDropPacket()) {
            return;
        }
        
        const char *err = NULL;
        const uint8_t *rawData(Binding::extractData(data)+offset);
        
        Conn *conn = findConn(remoteAddress);
        
        EmiPacketHeader packetHeader;
        size_t packetHeaderLength;
        if (!EmiPacketHeader::parse(rawData, len, &packetHeader, &packetHeaderLength)) {
            err = "Invalid packet header";
            goto error;
        }
        
        if (conn) {
            conn->gotPacket(remoteAddress, packetHeader, now);
        }
        
        if (packetHeaderLength == len) {
            // This is a heartbeat packet. Just forward the packet (if we can)
            if (conn) {
                conn->forwardPacket(now, inboundAddress, remoteAddress, data, offset, len);
            }
        }
        else if (len < packetHeaderLength + EMI_MESSAGE_HEADER_MIN_LENGTH) {
            err = "Packet too short";
            goto error;
        }
        else {
            EmiMessageHeader header;
            if (!EmiMessageHeader::parse(rawData+packetHeaderLength,
                                         len-packetHeaderLength,
                                         header)) {
                err = "Invalid message header";
                goto error;
            }
            
            const uint8_t *msgData   = rawData+packetHeaderLength+header.headerLength;
            const size_t   msgLength = header.length;
            
            bool isControlMessage = !!(header.flags & (EMI_PRX_FLAG | EMI_RST_FLAG | EMI_SYN_FLAG));
            
            size_t expectedPacketLength = packetHeaderLength+header.headerLength+header.length;
            bool isTheOnlyMessageInThisPacket = (len == expectedPacketLength);
            
            if (isControlMessage) {
                if (!isTheOnlyMessageInThisPacket) {
                    // This check also ensures that we don't buffer overflow
                    // when we access the message's data part.
                    err = "Invalid message length";
                    goto error;
                }
                
                // EMI_SACK_FLAG counts as a relevant flag because a control
                // message with this flag is an invalid flag, and counting it
                // as relevant makes sure that it is interpreted as an invalid
                // message.
                EmiMessageFlags relevantFlags = (header.flags & (EMI_PRX_FLAG | EMI_RST_FLAG |
                                                                 EMI_SYN_FLAG | EMI_ACK_FLAG |
                                                                 EMI_SACK_FLAG));
                
                if (EMI_SYN_FLAG == relevantFlags) {
                    // This is a connection open message.
                    gotConnectionOpen(now,
                                      header.sequenceNumber,
                                      rawData,
                                      len,
                                      sock,
                                      inboundAddress,
                                      remoteAddress,
                                      msgData, msgLength);
                }
                else if ((EMI_PRX_FLAG | EMI_ACK_FLAG) == relevantFlags) {
                    // This is a connection open ACK message.
                    if (conn && conn->isInitialSequenceNumberMismatch(remoteAddress, header.sequenceNumber)) {
                        err = "Got PRX-ACK message with unexpected sequence number";
                        goto error;
                    }
                    else if (conn) {
                        gotConnectionOpenAck(inboundAddress, remoteAddress,
                                             conn, now,
                                             msgData, msgLength);
                    }
                    else {
                        err = "Got PRX-ACK message without open conection";
                        goto error;
                    }
                }
                else if (EMI_RST_FLAG == relevantFlags) {
                    // This is a non-proxy connection close message.
                    if (conn) {
                        // We still have an open connection. This means that
                        // we haven't yet received confirmation from the other
                        // host that it has received the RST message, so we
                        // forward it.
                        conn->forwardPacket(now, inboundAddress, remoteAddress, data, offset, len);
                    }
                    else {
                        // We don't have an open connection. This probably means
                        // that the other host has already acknowledged the
                        // connection close and that the host that sent this packet
                        // did not receive the confirmation.
                        //
                        // In this case, we simply respond with a SYN-RST-ACK packet.
                        
                        EmiMessageFlags responseFlags(EMI_SYN_FLAG | EMI_RST_FLAG | EMI_ACK_FLAG);
                        uint8_t buf[96];
                        size_t size = EmiMessage<Binding>::writeControlPacket(responseFlags, buf, sizeof(buf));
                        ASSERT(0 != size); // size == 0 when the buffer was too small
                        
                        _socket->sendData(inboundAddress, remoteAddress, buf, size);
                    }
                }
                else if ((EMI_RST_FLAG | EMI_SYN_FLAG | EMI_ACK_FLAG) == relevantFlags) {
                    // This is a non-proxy connection close ack message.
                    if (conn) {
                        // We still have an open connection. This means that
                        // both hosts now know that the connection is closed,
                        // so we can forget about it.
                        //
                        // But before we do so, we want to forward this packet.
                        // (Not forwarding this packet will only force the other
                        // host to resend the RST-ACK message, in which case we
                        // will respond with SYN-RST-ACK anyways, but that is slower
                        // than immediately forwarding this packet)
                        conn->forwardPacket(now, inboundAddress, remoteAddress, data, offset, len);
                        
                        removeConnection(conn);
                    }
                    else {
                        // We don't have an open connection. In this case we don't
                        // need to do anything.
                    }
                }
                else if ((EMI_PRX_FLAG | EMI_RST_FLAG) == relevantFlags) {
                    // This is a proxy connection close message.
                    //
                    // Forget about the connection and respond with a PRX-RST-ACK packet.
                    
                    // Note that conn might very well be NULL, but it doesn't matter
                    removeConnection(conn);
                    
                    EmiMessageFlags responseFlags(EMI_PRX_FLAG | EMI_RST_FLAG | EMI_ACK_FLAG);
                    uint8_t buf[96];
                    size_t size = EmiMessage<Binding>::writeControlPacket(responseFlags, buf, sizeof(buf));
                    ASSERT(0 != size); // size == 0 when the buffer was too small
                    
                    _socket->sendData(inboundAddress, remoteAddress, buf, size);
                }
                else {
                    err = "Invalid message flags";
                    goto error;
                }
            }
            else {
                // This is not a control message, so we don't care about its
                // contents. Just forward it.
                if (conn) {
                    conn->forwardPacket(now, inboundAddress, remoteAddress, data, offset, len);
                }
            }
        }
        
        return;
    error:
        
        return;
    }
    
    const sockaddr_storage& getAddress() const {
        return _address;
    }
};

#endif
