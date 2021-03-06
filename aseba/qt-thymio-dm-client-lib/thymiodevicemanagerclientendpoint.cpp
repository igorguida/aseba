#include "thymiodevicemanagerclientendpoint.h"
#include <QDataStream>
#include <QtEndian>
#include <array>
#include <QtGlobal>
#include <QRandomGenerator>
#include "qflatbuffers.h"
#include "thymio-api.h"

namespace mobsya {

ThymioDeviceManagerClientEndpoint::ThymioDeviceManagerClientEndpoint(QTcpSocket* socket, QObject* parent)
    : QObject(parent), m_socket(socket), m_message_size(0) {

    connect(m_socket, &QTcpSocket::readyRead, this, &ThymioDeviceManagerClientEndpoint::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &ThymioDeviceManagerClientEndpoint::disconnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &ThymioDeviceManagerClientEndpoint::cancelAllRequests);
    connect(m_socket, &QTcpSocket::connected, this, &ThymioDeviceManagerClientEndpoint::onConnected);
    connect(m_socket, qOverload<QAbstractSocket::SocketError>(&QAbstractSocket::error), this,
            &ThymioDeviceManagerClientEndpoint::cancelAllRequests);
}

ThymioDeviceManagerClientEndpoint::~ThymioDeviceManagerClientEndpoint() {
    cancelAllRequests();
}

void ThymioDeviceManagerClientEndpoint::write(const flatbuffers::DetachedBuffer& buffer) {
    uint32_t size = buffer.size();
    m_socket->write(reinterpret_cast<const char*>(&size), sizeof(size));
    m_socket->write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    m_socket->flush();
}

void ThymioDeviceManagerClientEndpoint::write(const tagged_detached_flatbuffer& buffer) {
    write(buffer.buffer);
}


QHostAddress ThymioDeviceManagerClientEndpoint::peerAddress() const {
    return m_socket->peerAddress();
}

void ThymioDeviceManagerClientEndpoint::setWebSocketMatchingPort(quint16 port) {
    m_ws_port = port;
}

QUrl ThymioDeviceManagerClientEndpoint::websocketConnectionUrl() const {
    QUrl u;
    if(!m_ws_port)
        return {};
    u.setScheme("ws");
    u.setHost(m_socket->peerAddress().toString());
    u.setPort(m_ws_port);
    return u;
}

void ThymioDeviceManagerClientEndpoint::onReadyRead() {
    while(true) {
        if(m_message_size == 0) {
            if(m_socket->bytesAvailable() < 4)
                return;
            char data[4];
            m_socket->read(data, 4);
            m_message_size = *reinterpret_cast<quint32*>(data);
        }
        if(m_socket->bytesAvailable() < m_message_size)
            return;
        std::vector<uint8_t> data;
        data.resize(m_message_size);
        auto s = m_socket->read(reinterpret_cast<char*>(data.data()), m_message_size);
        Q_ASSERT(s == m_message_size);
        m_message_size = 0;
        flatbuffers::Verifier verifier(data.data(), data.size());
        if(!fb::VerifyMessageBuffer(verifier)) {
            qWarning() << "Invalid incomming message";
        }
        handleIncommingMessage(fb_message_ptr(std::move(data)));
    }
}

std::shared_ptr<ThymioNode> ThymioDeviceManagerClientEndpoint::node(const QUuid& id) const {
    return m_nodes.value(id);
}

void ThymioDeviceManagerClientEndpoint::handleIncommingMessage(const fb_message_ptr& msg) {
    switch(msg.message_type()) {
        case mobsya::fb::AnyMessage::NodesChanged: {
            auto message = msg.as<mobsya::fb::NodesChanged>();
            onNodesChanged(*(message->UnPack()));
            break;
        }
        case mobsya::fb::AnyMessage::RequestCompleted: {
            auto message = msg.as<mobsya::fb::RequestCompleted>();
            auto basic_req = get_request(message->request_id());
            if(!basic_req)
                break;
            if(auto req = basic_req->as<Request::internal_ptr_type>()) {
                req->setResult();
            }
            break;
        }
        case mobsya::fb::AnyMessage::Error: {
            auto message = msg.as<mobsya::fb::Error>();
            auto basic_req = get_request(message->request_id());
            if(!basic_req)
                break;
            basic_req->setError(message->error());
            break;
        }
        case mobsya::fb::AnyMessage::CompilationResultFailure: {
            auto message = msg.as<mobsya::fb::CompilationResultFailure>();
            auto basic_req = get_request(message->request_id());
            if(!basic_req)
                break;
            if(auto req = basic_req->as<CompilationRequest::internal_ptr_type>()) {
                auto r = CompilationResult::make_error(QString(message->message()->c_str()), message->character(),
                                                       message->line(), message->column());
                req->setResult(std::move(r));
            }
            break;
        }
        case mobsya::fb::AnyMessage::CompilationResultSuccess: {
            auto message = msg.as<mobsya::fb::CompilationResultSuccess>();
            auto basic_req = get_request(message->request_id());
            if(!basic_req)
                break;
            if(auto req = basic_req->as<CompilationRequest::internal_ptr_type>()) {
                auto r =
                    CompilationResult::make_success(message->bytecode_size(), message->variables_size(),
                                                    message->total_bytecode_size(), message->total_variables_size());
                req->setResult(std::move(r));
            }
            break;
        }

        case mobsya::fb::AnyMessage::SetBreakpointsResponse: {
            auto message = msg.as<mobsya::fb::SetBreakpointsResponse>();
            auto basic_req = get_request(message->request_id());
            if(!basic_req)
                break;
            if(auto req = basic_req->as<BreakpointsRequest::internal_ptr_type>()) {
                const fb::SetBreakpointsResponseT& r = *(message->UnPack());
                QVector<unsigned> breakpoints;
                for(const auto& bp : r.breakpoints) {
                    if(bp)
                        breakpoints.append(bp->line);
                }
                req->setResult(SetBreakpointRequestResult(breakpoints));
            }
            break;
        }

        case mobsya::fb::AnyMessage::VMExecutionStateChanged: {
            auto message = msg.as<mobsya::fb::VMExecutionStateChanged>()->UnPack();
            if(!message)
                break;
            auto id = qfb::uuid(message->node_id->id);
            if(auto node = m_nodes.value(id)) {
                node->onExecutionStateChanged(*message);
            }
            break;
        }

        case mobsya::fb::AnyMessage::NodeVariablesChanged: {
            auto message = msg.as<mobsya::fb::NodeVariablesChanged>();
            if(!message || !message->vars())
                break;
            auto id = qfb::uuid(message->node_id()->UnPack());
            auto node = m_nodes.value(id);
            if(!node)
                break;

            ThymioNode::VariableMap vars;
            for(const auto& var : *(message->vars())) {
                if(!var)
                    continue;
                auto name = qfb::as_qstring(var->name());
                if(name.isEmpty())
                    continue;
                auto value = qfb::to_qvariant(var->value_flexbuffer_root());
                vars.insert(name, ThymioVariable(value, var->constant()));
            }
            node->onVariablesChanged(std::move(vars));
            break;
        }

        case mobsya::fb::AnyMessage::EventsEmitted: {
            auto message = msg.as<mobsya::fb::EventsEmitted>();
            if(!message || !message->events())
                break;
            auto id = qfb::uuid(message->node_id()->UnPack());
            auto node = m_nodes.value(id);
            if(!node)
                break;
            ThymioNode::VariableMap events;
            for(const auto& event : *(message->events())) {
                if(!event)
                    continue;
                auto name = qfb::as_qstring(event->name());
                if(name.isEmpty())
                    continue;
                auto value = qfb::to_qvariant(event->value_flexbuffer_root());
                events.insert(name, ThymioVariable(value, false));
            }
            node->onEvents(std::move(events));
            break;
        }
        case mobsya::fb::AnyMessage::EventsDescriptionChanged: {
            auto message = msg.as<mobsya::fb::EventsDescriptionChanged>();
            if(!message || !message->events())
                break;
            auto id = qfb::uuid(message->node_id()->UnPack());
            auto node = m_nodes.value(id);
            if(!node)
                break;
            QVector<mobsya::EventDescription> events;
            for(const auto& event : *(message->events())) {
                if(!event)
                    continue;
                auto name = qfb::as_qstring(event->name());
                if(name.isEmpty())
                    continue;
                auto size = event->fixed_sized();
                events.append(EventDescription(name, size));
            }
            node->onEventsTableChanged(std::move(events));
            break;
        }

        case mobsya::fb::AnyMessage::NodeAsebaVMDescription: {
            auto message = msg.as<mobsya::fb::NodeAsebaVMDescription>();
            if(!message)
                break;
            auto basic_req = get_request(message->request_id());
            if(!basic_req)
                break;
            auto req = basic_req->as<AsebaVMDescriptionRequest::internal_ptr_type>();
            if(!req)
                break;
            const fb::NodeAsebaVMDescriptionT& r = *(message->UnPack());
            req->setResult(AsebaVMDescriptionRequestResult(r));
            break;
        }
        default: Q_EMIT onMessage(msg);
    }
}

void ThymioDeviceManagerClientEndpoint::onNodesChanged(const fb::NodesChangedT& nodes) {
    for(const auto& ptr : nodes.nodes) {
        const fb::NodeT& node = *ptr;
        const QUuid id = qfb::uuid(*node.node_id);
        if(id.isNull())
            continue;
        ThymioNode::NodeCapabilities caps;
        if(node.capabilities & uint64_t(fb::NodeCapability::ForceResetAndStop))
            caps |= ThymioNode::NodeCapability::ForceResetAndStop;
        if(node.capabilities & uint64_t(fb::NodeCapability::Rename))
            caps |= ThymioNode::NodeCapability::Rename;
        QString name = QString::fromStdString(node.name);
        auto status = static_cast<ThymioNode::Status>(node.status);
        auto type = static_cast<ThymioNode::NodeType>(node.type);


        bool added = false;
        auto it = m_nodes.find(id);
        if(it == m_nodes.end()) {
            it = m_nodes.insert(id, std::make_shared<ThymioNode>(shared_from_this(), id, name, type));
            added = true;
        }

        (*it)->setName(name);
        (*it)->setStatus(status);
        (*it)->setCapabilities(caps);

        Q_EMIT added ? nodeAdded(it.value()) : nodeModified(it.value());

        if(status == ThymioNode::Status::Disconnected) {
            auto node = it.value();
            m_nodes.erase(it);
            Q_EMIT nodeRemoved(node);
        }
    }
}

detail::RequestDataBase::shared_ptr
ThymioDeviceManagerClientEndpoint::get_request(detail::RequestDataBase::request_id id) {
    auto it = m_pending_requests.find(id);
    if(it == m_pending_requests.end())
        return {};
    auto res = *it;
    m_pending_requests.erase(it);
    return res;
}

void ThymioDeviceManagerClientEndpoint::cancelAllRequests() {
    for(auto&& r : m_pending_requests) {
        r->cancel();
    }
    m_pending_requests.clear();
}

void ThymioDeviceManagerClientEndpoint::onConnected() {
    flatbuffers::FlatBufferBuilder builder;
    fb::ConnectionHandshakeBuilder hsb(builder);
    hsb.add_protocolVersion(protocolVersion);
    hsb.add_minProtocolVersion(minProtocolVersion);
    write(wrap_fb(builder, hsb.Finish()));
}

flatbuffers::Offset<fb::NodeId> ThymioDeviceManagerClientEndpoint::serialize_uuid(flatbuffers::FlatBufferBuilder& fb,
                                                                                  const QUuid& uuid) {
    std::array<uint8_t, 16> data;
    *reinterpret_cast<uint32_t*>(data.data()) = qFromBigEndian(uuid.data1);
    *reinterpret_cast<uint16_t*>(data.data() + 4) = qFromBigEndian(uuid.data2);
    *reinterpret_cast<uint16_t*>(data.data() + 6) = qFromBigEndian(uuid.data3);
    memcpy(data.data() + 8, uuid.data4, 8);

    return mobsya::fb::CreateNodeId(fb, fb.CreateVector(data.data(), data.size()));
}

Request ThymioDeviceManagerClientEndpoint::renameNode(const ThymioNode& node, const QString& newName) {

    Request r = prepare_request<Request>();
    flatbuffers::FlatBufferBuilder builder;
    auto uuidOffset = serialize_uuid(builder, node.uuid());
    auto nameOffset = qfb::add_string(builder, newName);
    fb::RenameNodeBuilder rn(builder);
    rn.add_request_id(r.id());
    rn.add_node_id(uuidOffset);
    rn.add_new_name(nameOffset);
    write(wrap_fb(builder, rn.Finish()));
    return r;
}

Request ThymioDeviceManagerClientEndpoint::setNodeExecutionState(const ThymioNode& node,
                                                                 fb::VMExecutionStateCommand cmd) {
    Request r = prepare_request<Request>();
    flatbuffers::FlatBufferBuilder builder;
    auto uuidOffset = serialize_uuid(builder, node.uuid());
    write(wrap_fb(builder, fb::CreateSetVMExecutionState(builder, r.id(), uuidOffset, cmd)));
    return r;
}

BreakpointsRequest ThymioDeviceManagerClientEndpoint::setNodeBreakPoints(const ThymioNode& node,
                                                                         const QVector<unsigned>& breakpoints) {
    BreakpointsRequest r = prepare_request<BreakpointsRequest>();
    flatbuffers::FlatBufferBuilder builder;
    auto uuidOffset = serialize_uuid(builder, node.uuid());
    std::vector<flatbuffers::Offset<fb::Breakpoint>> serialized_breakpoints;
    serialized_breakpoints.reserve(breakpoints.size());
    std::transform(breakpoints.begin(), breakpoints.end(), std::back_inserter(serialized_breakpoints),
                   [&builder](const auto bp) { return fb::CreateBreakpoint(builder, bp); });
    auto vecOffset = builder.CreateVector(serialized_breakpoints);
    write(wrap_fb(builder, fb::CreateSetBreakpoints(builder, r.id(), uuidOffset, vecOffset)));
    return r;
}

auto ThymioDeviceManagerClientEndpoint::lock(const ThymioNode& node) -> Request {
    Request r = prepare_request<Request>();
    flatbuffers::FlatBufferBuilder builder;
    auto uuidOffset = serialize_uuid(builder, node.uuid());
    write(wrap_fb(builder, fb::CreateLockNode(builder, r.id(), uuidOffset)));
    return r;
}
auto ThymioDeviceManagerClientEndpoint::unlock(const ThymioNode& node) -> Request {
    Request r = prepare_request<Request>();
    flatbuffers::FlatBufferBuilder builder;
    auto uuidOffset = serialize_uuid(builder, node.uuid());
    write(wrap_fb(builder, fb::CreateUnlockNode(builder, r.id(), uuidOffset)));
    return r;
}

auto ThymioDeviceManagerClientEndpoint::send_code(const ThymioNode& node, const QByteArray& code,
                                                  fb::ProgrammingLanguage language, fb::CompilationOptions opts)
    -> CompilationRequest {

    CompilationRequest r = prepare_request<CompilationRequest>();
    flatbuffers::FlatBufferBuilder builder;
    auto uuidOffset = serialize_uuid(builder, node.uuid());
    auto codedOffset = builder.CreateString(code.data(), code.size());
    write(wrap_fb(builder, fb::CreateCompileAndLoadCodeOnVM(builder, r.id(), uuidOffset, language, codedOffset, opts)));
    return r;
}

Request ThymioDeviceManagerClientEndpoint::set_watch_flags(const ThymioNode& node, int flags) {
    Request r = prepare_request<Request>();
    flatbuffers::FlatBufferBuilder builder;
    auto uuidOffset = serialize_uuid(builder, node.uuid());
    write(wrap_fb(builder, fb::CreateWatchNode(builder, r.id(), uuidOffset, flags)));
    return r;
}

AsebaVMDescriptionRequest ThymioDeviceManagerClientEndpoint::fetchAsebaVMDescription(const ThymioNode& node) {
    AsebaVMDescriptionRequest r = prepare_request<AsebaVMDescriptionRequest>();
    flatbuffers::FlatBufferBuilder builder;
    auto uuidOffset = serialize_uuid(builder, node.uuid());
    write(wrap_fb(builder, fb::CreateRequestNodeAsebaVMDescription(builder, r.id(), uuidOffset)));
    return r;
}

namespace detail {
    auto serialize_variables(flatbuffers::FlatBufferBuilder& fb, const ThymioNode::VariableMap& vars) {
        flexbuffers::Builder flexbuilder;
        std::vector<flatbuffers::Offset<fb::NodeVariable>> varsOffsets;
        varsOffsets.reserve(vars.size());
        for(auto&& var : vars.toStdMap()) {
            qfb::to_flexbuffer(var.second.value(), flexbuilder);
            auto& vec = flexbuilder.GetBuffer();
            auto vecOffset = fb.CreateVector(vec);
            auto keyOffset = qfb::add_string(fb, var.first);
            varsOffsets.push_back(fb::CreateNodeVariable(fb, keyOffset, vecOffset, var.second.isConstant()));
            flexbuilder.Clear();
        }
        return fb.CreateVectorOfSortedTables(&varsOffsets);
    }
}  // namespace detail

Request ThymioDeviceManagerClientEndpoint::setNodeVariabes(const ThymioNode& node,
                                                           const ThymioNode::VariableMap& vars) {
    Request r = prepare_request<Request>();
    flatbuffers::FlatBufferBuilder builder;
    auto uuidOffset = serialize_uuid(builder, node.uuid());
    auto varsOffset = detail::serialize_variables(builder, vars);
    write(wrap_fb(builder, fb::CreateSetNodeVariables(builder, r.id(), uuidOffset, varsOffset)));
    return r;
}

Request ThymioDeviceManagerClientEndpoint::setNodeEventsTable(const ThymioNode& node,
                                                              const QVector<EventDescription>& events) {
    Request r = prepare_request<Request>();
    flatbuffers::FlatBufferBuilder builder;
    auto uuidOffset = serialize_uuid(builder, node.uuid());
    std::vector<flatbuffers::Offset<fb::EventDescription>> descOffsets;
    descOffsets.reserve(events.size());
    int i = 0;
    for(auto&& desc : events) {
        auto str_offset = qfb::add_string(builder, desc.name());
        auto descTable = fb::CreateEventDescription(builder, str_offset, desc.size(), i++);
        descOffsets.push_back(descTable);
    }
    write(wrap_fb(builder, fb::CreateRegisterEvents(builder, r.id(), uuidOffset, builder.CreateVector(descOffsets))));
    return r;
}

Request ThymioDeviceManagerClientEndpoint::emitNodeEvents(const ThymioNode& node,
                                                          const ThymioNode::VariableMap& events) {
    Request r = prepare_request<Request>();
    flatbuffers::FlatBufferBuilder builder;
    auto uuidOffset = serialize_uuid(builder, node.uuid());
    auto eventsOffset = detail::serialize_variables(builder, events);
    write(wrap_fb(builder, fb::CreateSendEvents(builder, r.id(), uuidOffset, eventsOffset)));
    return r;
}

}  // namespace mobsya
