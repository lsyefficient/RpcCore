#pragma once

#include <map>
#include <memory>
#include <utility>

#include "base/noncopyable.hpp"
#include "Connection.hpp"
#include "coder/Coder.hpp"
#include "Message.hpp"
#include "log.h"

namespace RpcCore {

/**
 * 消息分发器
 * 注册消息到指定命令
 */
class MsgDispatcher : noncopyable {
public:
    using CmdHandle = std::function<MsgWrapper(MsgWrapper)>;
    using RspHandle = std::function<void(MsgWrapper)>;

    using TimeoutCb = std::function<void()>;
    using SetTimeout = std::function<void(uint32_t ms, TimeoutCb)>;

public:
    explicit MsgDispatcher(std::shared_ptr<Connection> conn, std::shared_ptr<coder::Coder> coder)
            : conn_(std::move(conn)), coder_(std::move(coder)) {
        conn_->setRecvPacketCb([this](const std::string& payload){
            bool success;
            auto msg = coder_->unserialize(payload, success);
            if (success) {
                this->dispatch(msg);
            } else {
                LOGE("payload can not be parsed, msg info");
            }
        });

        // 每一个连接要册一个PING消息，以便有PING到来时，给发送者回复PONG，PING/PONG可携带payload，会原样返回。
        subscribeCmd(InnerCmd::PING, [](MsgWrapper msg) {
            msg.cmd = InnerCmd::PONG;
            return MsgWrapper::MakeRsp(msg.seq, msg.unpackAs<String>());
        });
    }

public:
    void dispatch(const MsgWrapper& msg)
    {
        switch (msg.type) {
            case MsgWrapper::COMMAND:
            {
                // COMMAND
                const auto& cmd = msg.cmd;
                LOGD("dispatch cmd:%s, seq:%d, conn:%p", CmdToStr(cmd).c_str(), msg.seq, conn_.get());

                auto it = cmdHandleMap_.find(cmd);
                if (it == cmdHandleMap_.cend()) {
                    LOGD("not register cmd for: %s", CmdToStr(cmd).c_str());
                    return;
                }
                const auto& fn = (*it).second;
                auto resp = fn(msg);
                conn_->sendPacket(coder_->serialize(resp));
            } break;

            case MsgWrapper::RESPONSE:
            {
                LOGD("dispatch rsp: seq=%d, conn:%p", msg.seq, conn_.get());
                auto it = rspHandleMap_.find(msg.seq);
                if (it == rspHandleMap_.cend()) {
                    LOGD("not register callback for response");
                    break;
                }
                const auto& cb = (*it).second;
                if(not cb) {
                    LOGE("rsp handle can not be null");
                    return;
                }
                cb(msg);
                rspHandleMap_.erase(it);
                LOGD("rspHandleMap_.size=%zu", rspHandleMap_.size());
            } break;

            default:
                LOGE("unknown message type, conn:%p", conn_.get());
        }
    }

    inline void subscribeCmd(CmdType cmd, CmdHandle handle)
    {
        LOGD("subscribeCmd cmd:%s, conn:%p, handle:%p", CmdToStr(cmd).c_str(), conn_.get(), &handle);
        cmdHandleMap_[std::move(cmd)] = std::move(handle);
    }

    void unsubscribeCmd(const CmdType& cmd)
    {
        auto it = cmdHandleMap_.find(cmd);
        if (it != cmdHandleMap_.cend()) {
            LOGD("erase cmd: %s", CmdToStr(cmd).c_str());
            cmdHandleMap_.erase(it);
        } else {
            LOGD("not register cmd for: %s", CmdToStr(cmd).c_str());
        }
    }

    void subscribeRsp(SeqType seq, RspHandle handle, const TimeoutCb& timeoutCb, uint32_t timeoutMs)
    {
        LOGD("subscribeRsp seq:%d, handle:%p", seq, &handle);
        if (handle == nullptr) return;
        rspHandleMap_[seq] = std::move(handle);

        if(setTimeout_ == nullptr) {
            LOGW("no timeout will cause memory leak!");
        }

        setTimeout_(timeoutMs, [this, seq, timeoutCb] {
            auto it = rspHandleMap_.find(seq);
            if (it != rspHandleMap_.cend()) {
                if (timeoutCb) {
                    timeoutCb();
                }
                rspHandleMap_.erase(seq);
                LOGD("Timeout seq=%d, rspHandleMap_.size=%zu", seq, rspHandleMap_.size());
            }
        });
    }

    inline std::shared_ptr<Connection> getConn() const
    {
        return conn_;
    }

    inline void setTimerFunc(SetTimeout timerFunc)
    {
        setTimeout_ = std::move(timerFunc);
    }

private:
    std::shared_ptr<Connection> conn_;
    std::shared_ptr<coder::Coder> coder_;
    std::map<CmdType, CmdHandle> cmdHandleMap_;
    std::map<SeqType, RspHandle> rspHandleMap_;
    SetTimeout setTimeout_;
};

}
