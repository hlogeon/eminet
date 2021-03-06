#define BUILDING_NODE_EXTENSION
#ifndef eminet_EmiConnDelegate_h
#define eminet_EmiConnDelegate_h

#include "../core/EmiTypes.h"
#include <uv.h>
#include <node.h>

class EmiConnection;
class EmiObjectWrap;

class EmiConnDelegate {
    EmiConnection& _conn;
    
public:
    EmiConnDelegate(EmiConnection& conn);
    
    void invalidate();
    
    void emiConnPacketLoss(EmiChannelQualifier channelQualifier,
                           EmiSequenceNumber packetsLost);
    void emiConnMessage(EmiChannelQualifier channelQualifier,
                        const v8::Local<v8::Object>& data,
                        size_t offset,
                        size_t size);
    
    void scheduleConnectionWarning(EmiTimeInterval warningTimeout);
    
    void emiConnLost();
    void emiConnRegained();
    void emiConnDisconnect(EmiDisconnectReason reason);
    void emiNatPunchthroughFinished(bool success);
    
    inline EmiConnection& getConnection() { return _conn; }
    inline const EmiConnection& getConnection() const { return _conn; }
    
    inline EmiObjectWrap *getSocketCookie() { return (EmiObjectWrap *)&_conn; }
    inline void *getTimerCookie() { return NULL; }
};

#endif
