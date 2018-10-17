#pragma once
#include <QObject>
#include <QEventLoop>
#include <QCoreApplication>
#include <QRandomGenerator>
#include <memory>
#include <optional>
#include <mutex>
#include <aseba/flatbuffers/fb_message_ptr.h>
#include <QtDebug>
#include <iostream>

namespace mobsya {

class Error {
    Q_GADGET

public:
    using E = mobsya::fb::ErrorType;
    Q_ENUM(E);

public:
    Error(E e = E::no_error) : m_error(e) {}

    Q_INVOKABLE E error() const {
        return m_error;
    }

    Q_INVOKABLE operator E() const {
        return m_error;
    }

    Q_INVOKABLE QString toString() {
        switch(m_error) {
            case E::node_busy: return "Node busy";
            case E::unknown_node: return "Unknown node";
            case E::unsupported_variable_type: return "Unsupported variable type";
            case E::unknown_error: return "Unknown error";
            case E::no_error: break;
        }
        return {};
    }

private:
    E m_error;
};

namespace detail {

    class RequestDataBase : public QObject {
        Q_OBJECT
    public:
        using request_id = quint32;
        using shared_ptr = std::shared_ptr<RequestDataBase>;

        RequestDataBase(quint32 id, quint32 type) : m_id(id), m_type(type) {
            m_canceled.store(false);
        }
        void cancel() {
            m_canceled.store(true);
            Q_EMIT finished();
            Q_EMIT canceled();
        }
        bool isCanceled() const {
            return m_canceled;
        }

        quint32 id() const {
            return m_id;
        }

        quint32 type() const {
            return m_type;
        }

        template <typename... Args>
        void setError(Args&&... args) {
            {
                std::unique_lock<std::mutex> _(m_mutex);
                m_error = Error(std::forward<Args>(args)...);
            }
            Q_EMIT finished();
        }

        void setError(Error r) {
            {
                std::unique_lock<std::mutex> _(m_mutex);
                m_error = std::move(r);
            }
            Q_EMIT finished();
        }
        Error getError() {
            std::unique_lock<std::mutex> _(m_mutex);
            if(m_error)
                return *m_error;
            return {};
        }

        template <typename T>
        const T* as() const {
            if(T::type != m_type)
                return {};
            return static_cast<const T*>(this);
        }

        template <typename T>
        T* as() {
            if(T::type != m_type)
                return {};
            return static_cast<T*>(this);
        }

    Q_SIGNALS:
        void finished();
        void canceled();

    protected:
        mutable std::mutex m_mutex;
        const quint32 m_id;
        const quint32 m_type;
        std::atomic_bool m_canceled;
        std::optional<Error> m_error;
        friend class ThymioDeviceManagerClientEndpoint;
    };

    template <typename Result>
    class RequestData : public RequestDataBase {
    public:
        static constexpr auto type = Result::type;
        using RequestDataBase::RequestDataBase;

        void setResult(Result r) {
            {
                std::unique_lock<std::mutex> _(m_mutex);
                m_data = std::move(r);
            }
            Q_EMIT finished();
        }

        template <typename... Args>
        void setResult(Args&&... args) {
            {
                std::unique_lock<std::mutex> _(m_mutex);
                m_data = Result(std::forward<Args>(args)...);
            }
            Q_EMIT finished();
        }

        Result getResult() {
            if(!isFinished())
                wait();
            std::unique_lock<std::mutex> _(m_mutex);
            return *m_data;
        }


        bool isFinished() const {
            std::unique_lock<std::mutex> _(m_mutex);
            return m_canceled || m_error || m_data;
        }

        bool success() const {
            std::unique_lock<std::mutex> _(m_mutex);
            return !m_canceled && m_data;
        }

        void wait() {
            while(!isFinished()) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            }
        }

    private:
        std::optional<Result> m_data;
    };
}  // namespace detail

template <typename Result>
class BasicRequest {
    friend class ThymioDeviceManagerClientEndpoint;

public:
    BasicRequest() {}
    BasicRequest(const BasicRequest& other) = default;
    virtual ~BasicRequest() {}
    using request_id = quint32;
    using internal_ptr_type = detail::RequestData<Result>;
    static request_id generate_request_id() {
        quint32 value = QRandomGenerator::global()->generate();
        return value;
    }

private:
    std::shared_ptr<detail::RequestData<Result>> get_ptr() const {
        return ptr;
    }
    static BasicRequest make_request() {
        BasicRequest<Result> r;
        r.ptr = std::make_shared<detail::RequestData<Result>>(generate_request_id(), Result::type);
        return r;
    }

public:
    request_id id() const {
        return ptr ? ptr->id() : 0;
    }

    Result getResult() const {
        return ptr->getResult();
    }

    Error getError() const {
        return ptr->getError();
    }

    bool success() const {
        return ptr && ptr->success();
    }

    void wait() {
        if(!ptr)
            return;
        return ptr->wait();
    }

    bool isCanceled() const {
        return !ptr || ptr->isCanceled();
    }

    bool isFinished() const {
        return !ptr || ptr->isFinished();
    }

private:
    template <typename R>
    friend class BasicRequestWatcher;
    std::shared_ptr<detail::RequestData<Result>> ptr;
};

namespace detail {
    class RequestWatcherBase : public QObject {
        Q_OBJECT

    Q_SIGNALS:
        void finished();
        void canceled();
    };

}  // namespace detail

template <typename Result>
class BasicRequestWatcher : public detail::RequestWatcherBase, public BasicRequest<Result> {
public:
    BasicRequestWatcher(const BasicRequest<Result>& req) : BasicRequest<Result>(req) {
        connect(this->get_ptr().get(), &detail::RequestDataBase::finished, this, &BasicRequestWatcher<Result>::finished,
                Qt::QueuedConnection);
        connect(this->get_ptr().get(), &detail::RequestDataBase::canceled, this, &BasicRequestWatcher<Result>::canceled,
                Qt::QueuedConnection);
    }
};


struct SimpleRequestResult {
    Q_GADGET

public:
    static constexpr quint32 type = 0x8543ec0d;
    Q_INVOKABLE QString toString() {
        return {};
    }
};
using Request = BasicRequest<SimpleRequestResult>;
using RequestWatcher = BasicRequestWatcher<SimpleRequestResult>;


}  // namespace mobsya