#pragma once

#include <memory>
#include <utility>

#ifdef RpcCore_THREAD_SUPPORT
#include <mutex>
#endif

#include "base/noncopyable.hpp"
#include "MsgWrapper.hpp"

namespace RpcCore {

#define RpcCore_Request_MAKE_PROP(type, name) \
        private: \
        type name##_; \
        public: \
        type name() { \
            return name##_; \
        } \
        std::shared_ptr<Request> name(type name) { \
            name##_ = std::move(name); \
            return shared_from_this(); \
        }

struct Request : noncopyable, public std::enable_shared_from_this<Request> {
    using SRequest = std::shared_ptr<Request>;
    using WRequest = std::weak_ptr<Request>;

    struct RpcProto {
        virtual ~RpcProto() = default;
        virtual SeqType makeSeq() = 0;
        virtual void sendRequest(const SRequest&) = 0;
    };
    using SSendProto = std::shared_ptr<RpcProto>;
    using WSendProto = std::weak_ptr<RpcProto>;

    struct DisposeProto {
        virtual ~DisposeProto() = default;
        virtual SRequest addRequest(SRequest request) = 0;
        virtual SRequest rmRequest(SRequest request) = 0;
    };
    using SDisposeProto = std::shared_ptr<DisposeProto>;
    using WDisposeProto = std::weak_ptr<DisposeProto>;

    enum class FinallyType {
        NORMAL,
        TIMEOUT,
        CANCELED,
        RPC_EXPIRED,
    };

    RpcCore_Request_MAKE_PROP(WSendProto, rpc);
    RpcCore_Request_MAKE_PROP(CmdType, cmd);
    RpcCore_Request_MAKE_PROP(void*, target);
    RpcCore_Request_MAKE_PROP(SeqType, seq);
    RpcCore_Request_MAKE_PROP(std::function<bool(MsgWrapper)>, rspHandle);
    RpcCore_Request_MAKE_PROP(uint32_t, timeoutMs);
    RpcCore_Request_MAKE_PROP(bool, canceled);
    RpcCore_Request_MAKE_PROP(std::function<void(FinallyType)>, finally);
    RpcCore_Request_MAKE_PROP(std::string, payload);

public:
    std::function<void()> timeoutCb_;
    std::shared_ptr<Request> timeout(std::function<void()> timeoutCb) {
        timeoutCb_ = [this, RpcCore_MOVE(timeoutCb)]{
            if (timeoutCb) {
                timeoutCb();
            }
            onFinish(FinallyType::TIMEOUT);

            if (retryCount_ == -1) {
                call();
            } else if (retryCount_ > 0) {
                retryCount_--;
                call();
            }
        };
        return shared_from_this();
    }

private:
    FinallyType finallyType_;
    void onFinish(FinallyType type, bool byDispose = false) {
        LOGV("onFinish: cmd:%s, %p", cmd().c_str(), this);
        if (not dispose_.expired() && not byDispose) {
            if (type == FinallyType::TIMEOUT && retryCount_ != 0) { // will retry
            } else {
                dispose_.lock()->rmRequest(shared_from_this());
            }
        }
        finallyType_ = type;
        if (finally_) {
            finally_(finallyType_);
        }
        notify();
    }

    inline void notify() {
#ifdef RpcCore_THREAD_SUPPORT
        cond_.notify_one();
#endif
    }

public:
    explicit Request(const SSendProto& rpc = nullptr) : rpc_(rpc) {}
    ~Request() {
        LOGD("~Request: cmd:%s, %p", RpcCore::CmdToStr(cmd_).c_str(), this);
    }

    static SRequest create(SSendProto rpc = nullptr) {
        auto request = std::make_shared<Request>(rpc);
        request->init();
        return request;
    }

    void init() {
        timeoutMs(3000);
        target(nullptr);
        canceled(false);
        timeout(nullptr);
        setCb(nullptr);
        inited_ = true;
    }

public:
    SRequest call(const SSendProto& rpc = nullptr) {
        assert(inited_);

        if (canceled()) return shared_from_this();

        auto self = shared_from_this();
        if (rpc) {
            rpc_ = rpc;
        } else if (rpc_.expired()) {
            onFinish(FinallyType::RPC_EXPIRED);
            return self;
        }
        auto r = rpc_.lock();
        seq_ = r->makeSeq();
        r->sendRequest(self);
        return self;
    }

    SRequest cancel(bool byDispose = false) {
        assert(inited_);
        canceled(true);
        onFinish(FinallyType::CANCELED, byDispose);
        return shared_from_this();
    }

    template<typename T, RpcCore_ENSURE_TYPE_IS_MESSAGE(T)>
    SRequest rsp(std::function<void(T&&)> cb) {
        auto self = shared_from_this();
        this->rspHandle_ = [this, RpcCore_MOVE(cb), self](MsgWrapper msg){
            if (canceled()) {
                onFinish(FinallyType::CANCELED);
                return true;
            }

            auto rsp = msg.unpackAs<T>();
            if (rsp.first) {
                if (cb) cb(std::forward<T>(rsp.second));
                onFinish(FinallyType::NORMAL);
                return true;
            }
            return false;
        };
        return self;
    }

    SRequest setCb(std::function<void()> cb) {
        auto self = shared_from_this();
        this->rspHandle_ = [this, RpcCore_MOVE(cb), self](const MsgWrapper&){
            if (canceled()) {
                onFinish(FinallyType::CANCELED);
                return true;
            }
            if (cb) cb();
            notify();
            return true;
        };
        return self;
    }

    SRequest msg(const Message& message) {
        this->payload(message.serialize());
        return shared_from_this();
    }

    template<typename T, typename R>
    SRequest setMessage(const R& message) {
        this->payload(((T)message).serialize());
        return shared_from_this();
    }

    /**
     * 添加到dispose并且当执行完成后 自动从dispose删除
     */
    SRequest addTo(const SDisposeProto& dispose) {
        dispose_ = dispose;
        auto self = shared_from_this();
        dispose->addRequest(self);
        return self;
    }

    /**
     * 超时后自动重试次数 -1为一直尝试 0为停止 >0为尝试次数
     */
    SRequest retryCount(int count) {
        retryCount_ = count;
        return shared_from_this();
    }

private:
    bool inited_ = false;
    WDisposeProto dispose_;
    int retryCount_ = 0;

#ifdef RpcCore_THREAD_SUPPORT // todo: 待实现 当前只可以在其他线程中等待
private:
    std::mutex mutex_;
    std::condition_variable cond_;
public:
    void wait() {
        std::unique_lock<std::mutex> guard(mutex_);
        cond_.wait(guard);
    }
#endif
};

using SRequest = Request::SRequest;
using FinishType = Request::FinallyType;

}
