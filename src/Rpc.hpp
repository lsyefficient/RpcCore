#pragma once

#include <memory>
#include <utility>

#include "base/noncopyable.hpp"
#include "Connection.hpp"
#include "MsgDispatcher.hpp"
#include "coder/JsonCoder.hpp"

#define RpcCore_TIMEOUT_PARAM const TimeoutCb& timeoutCb = nullptr, uint32_t timeoutMs = 3000

namespace RpcCore {

/**
 * 1. 封装操作细节以方便发送接收消息
 * 2. 持有一个连接 收发均基于此连接
 * 为了方便使用，消息注册和发送重载了多种形式。
 */
class Rpc : noncopyable {
    using CmdHandle = MsgDispatcher::CmdHandle;
    using RspHandle = MsgDispatcher::RspHandle;
    using TimeoutCb = MsgDispatcher::TimeoutCb;

public:
    explicit Rpc(
            std::shared_ptr<Connection> conn,
            std::shared_ptr<coder::Coder> coder = std::make_shared<coder::JsonCoder>())
            : conn_(conn), coder_(std::move(coder)), dispatcher_(std::move(conn), coder_)
    {
    }

public:
    inline std::shared_ptr<Connection> getConn() const {
        return conn_;
    }

    inline void setTimerImpl(MsgDispatcher::TimerImpl timerImpl) {
        dispatcher_.setTimerImpl(std::move(timerImpl));
    }

public:
    /**
     * 注册命令 接收消息 返回消息
     * @tparam T 接收消息的类型 这将决定解析行为 与发送时"发送参数类型"一致
     * @tparam R 返回消息结果的类型 与发送时"回调参数类型"一致
     * @param cmd
     * @param handle 接收T类型消息 返回T类型消息作为回复 可使用R(Message, bool)构造 也可直接返回Message或bool
     */
    template <typename T, typename R, RpcCore_ENSURE_TYPE_IS_MESSAGE(T), RpcCore_ENSURE_TYPE_IS_MESSAGE(R)>
    void subscribe(const CmdType& cmd, const std::function<R(T&&)>& handle) {
        dispatcher_.subscribeCmd(cmd, [handle](const MsgWrapper& msg) {
            auto r = msg.unpackAs<T>();
            if (r.first) {
                handle(std::move(r.second));
            }
            return MsgWrapper::MakeRsp(
                    msg.seq,
                    r.second,
                    r.first
            );
        });
    }

    /**
     * 注册命令 接收 无回复
     * @tparam T 接收消息的类型
     * @param cmd
     * @param handle 接收数据 返回操作状态
     */
    template <typename T, RpcCore_ENSURE_TYPE_IS_MESSAGE(T)>
    void subscribe(const CmdType& cmd, const std::function<void(T&&)>& handle) {
        dispatcher_.subscribeCmd(cmd, [handle](const MsgWrapper& msg) {
            auto r = msg.unpackAs<T>();
            if (r.first) {
                handle(std::move(r.second));
            }
            return MsgWrapper::MakeRsp(
                    msg.seq,
                    r.second,
                    r.first
            );
        });
    }

    /**
     * 注册命令 不接收 无回复
     * @param cmd
     * @param handle 不接收参数 返回操作状态
     */
    inline void subscribe(CmdType cmd, const std::function<void()>& handle) {
        dispatcher_.subscribeCmd(std::move(cmd), [handle](const MsgWrapper& msg) {
            handle();
            return MsgWrapper::MakeRsp(msg.seq);
        });
    }

    /**
     * 注册命令 不接收 有回复
     * @tparam R 返回给对方的数据类型
     * @param cmd
     * @param handle 不接收参数 返回R(msg, true)形式
     */
    template <typename R, RpcCore_ENSURE_TYPE_IS_MESSAGE(R)>
    inline void subscribe(const CmdType& cmd, const std::function<R()>& handle) {
        dispatcher_.subscribeCmd(cmd, [handle](const MsgWrapper& msg) {
            R rsp = handle();
            return MsgWrapper::MakeRsp(
                    msg.seq,
                    rsp
            );
        });
    }

    /**
     * 取消注册的命令
     * @param cmd
     */
    inline void unsubscribe(const CmdType& cmd) {
        dispatcher_.unsubscribeCmd(cmd);
    }

    /**
     * 发送命令和消息 有回复
     * @tparam T 消息回复数据载体的类型
     * @param cmd 消息类型
     * @param message 消息数据
     * @param cb 消息回调 参数类型T
     * @param timeoutCb 超时回调
     * @param timeoutMs 超时时间
     */
    template <typename T, RpcCore_ENSURE_TYPE_IS_MESSAGE(T)>
    inline void send(const CmdType& cmd, const Message& message, const std::function<void(T&&)>& cb, RpcCore_TIMEOUT_PARAM) {
        sendMessage(cmd, message, [cb](const MsgWrapper& msg) {
            auto r = msg.unpackAs<T>();
            if (r.first) {
                cb(std::move(r.second));
            }
            return r.first;
        }, timeoutCb, timeoutMs);
    }
    /**
     * 发送命令 获取回复消息
     * @tparam T 接收消息的类型
     * @param cmd
     * @param cb 消息回调 参数类型T
     * @param timeoutCb 超时回调
     * @param timeoutMs 超时时间
     */
    template <typename T, RpcCore_ENSURE_TYPE_IS_MESSAGE(T)>
    inline void send(const CmdType& cmd, const std::function<void(T&&)>& cb, RpcCore_TIMEOUT_PARAM) {
        sendMessage(cmd, VOID, [cb](const MsgWrapper& msg) {
            auto r = msg.unpackAs<T>();
            if (r.first) {
                cb(std::move(r.second));
            }
            return r.first;
        }, timeoutCb, timeoutMs);
    }

    /**
     * 发送命令和消息 获取是否成功
     * @param cmd
     * @param message
     * @param cb 消息回调 对方响应是否成功
     * @param timeoutCb 超时回调
     * @param timeoutMs 超时时间
     */
    inline void send(CmdType cmd, const Message& message, const std::function<void()>& cb = nullptr, RpcCore_TIMEOUT_PARAM) {
        sendMessage(std::move(cmd), message, [cb](const MsgWrapper& msg) {
            if (cb != nullptr) {
                cb();
            }
            return true;
        }, timeoutCb, timeoutMs);
    }

    /**
     * 发送命令 获取是否成功
     * @param cmd
     * @param cb
     * @param timeoutCb 超时回调
     * @param timeoutMs 超时时间
     */
    inline void send(CmdType cmd, const std::function<void()>& cb = nullptr, RpcCore_TIMEOUT_PARAM) {
        send(std::move(cmd), VOID, cb, timeoutCb, timeoutMs);
    }

    /**
     * 可作为连通性的测试 会原样返回payload
     * @param payload 负载数据默认为空
     * @param cb 参数类型std::string
     * @param timeoutCb 超时回调
     * @param timeoutMs 超时时间
     */
    inline void sendPing(std::string payload = "", const std::function<void(String)>& cb = nullptr, RpcCore_TIMEOUT_PARAM)
    {
        sendMessage(InnerCmd::PING, String(std::move(payload)), [cb](const MsgWrapper& msg) {
            if (cb == nullptr) return true;
            auto r = msg.unpackAs<String>();
            if (r.first) {
                cb(std::move(r.second));
            }
            return r.first;
        }, timeoutCb, timeoutMs);
    }

private:
    /**
     * 发送消息并设定回调
     * @param cmd
     * @param message
     * @param cb
     * @param timeoutCb 超时回调
     * @param timeoutMs 超时时间
     */
    inline void sendMessage(CmdType cmd, const Message& message = VOID, const RspHandle& cb = nullptr, RpcCore_TIMEOUT_PARAM)
    {
        // 指定消息类型创建payload
        conn_->sendPacket(CreateMessagePayload(std::move(cmd), message, cb, timeoutCb, timeoutMs));
    }

    /**
     * 创建消息并设置回调 返回Payload用于传输
     * @param cmd
     * @param message
     * @param cb
     * @param timeoutCb 超时回调
     * @param timeoutMs 超时时间
     * @return
     */
    inline std::string CreateMessagePayload(CmdType cmd, const Message& message = VOID, const RspHandle& cb = nullptr, RpcCore_TIMEOUT_PARAM)
    {
        auto msg = MsgWrapper::MakeCmd(std::move(cmd), seq_++, message);
        dispatcher_.subscribeRsp(msg.seq, cb, timeoutCb, timeoutMs);
        return coder_->serialize(msg);
    }

private:
    std::shared_ptr<Connection> conn_;
    std::shared_ptr<coder::Coder> coder_;
    MsgDispatcher dispatcher_;
    SeqType seq_{0};
};

}
