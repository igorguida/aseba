#pragma once
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <memory>
#include <type_traits>
#include <queue>
#include "flatbuffers_message_writer.h"
#include "flatbuffers_message_reader.h"
#include "flatbuffers_messages.h"
#include "aseba_node_registery.h"
#include "tdm.h"
#include "log.h"
#include "app_token_manager.h"
#include "utils.h"
#include <pugixml.hpp>

namespace mobsya {
using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;
using websocket_t = websocket::stream<tcp::socket>;

template <typename Self, typename Socket>
class application_endpoint_base : public std::enable_shared_from_this<application_endpoint_base<Self, Socket>> {
public:
    application_endpoint_base(boost::asio::io_context& ctx) = delete;
    template <typename CB>
    void read_message(CB&& handle) = delete;
    void start() = delete;
    void do_write_message(const flatbuffers::DetachedBuffer& buffer) = delete;
    tcp::socket& tcp_socket() = delete;
};

template <typename Self>
class application_endpoint_base<Self, websocket_t>
    : public std::enable_shared_from_this<application_endpoint_base<Self, websocket_t>> {
public:
    application_endpoint_base(boost::asio::io_context& ctx)
        : m_ctx(ctx), m_strand(ctx.get_executor()), m_socket(tcp::socket(ctx)) {}

    template <typename CB>
    void read_message(CB handle) {
        auto that = this->shared_from_this();


        auto cb = boost::asio::bind_executor(
            m_strand,
            [that, handle = std::move(handle)](boost::system::error_code ec, std::size_t bytes_transferred) mutable {
                if(ec) {
                    mLogError("read_message :{}", ec.message());
                    return;
                }
                std::vector<uint8_t> buf(boost::asio::buffers_begin(that->m_buffer.data()),
                                         boost::asio::buffers_begin(that->m_buffer.data()) + bytes_transferred);
                fb_message_ptr msg(std::move(buf));
                handle(ec, std::move(msg));
                that->m_buffer.consume(bytes_transferred);
            });
        m_socket.async_read(m_buffer, std::move(cb));
    }

    void do_write_message(const flatbuffers::DetachedBuffer& buffer) {
        auto that = this->shared_from_this();
        auto cb = boost::asio::bind_executor(m_strand, [that](boost::system::error_code ec, std::size_t s) {
            static_cast<Self&>(*that).handle_write(ec);
        });
        m_socket.async_write(boost::asio::buffer(buffer.data(), buffer.size()), std::move(cb));
    }
    void start() {
        m_socket.binary(true);
        auto that = this->shared_from_this();
        auto cb = boost::asio::bind_executor(
            m_strand, [that](boost::system::error_code ec) mutable { static_cast<Self&>(*that).on_initialized(ec); });
        m_socket.async_accept(std::move(cb));
    }

    tcp::socket& tcp_socket() {
        return m_socket.next_layer();
    }

protected:
    boost::asio::io_context& m_ctx;
    boost::asio::strand<boost::asio::io_context::executor_type> m_strand;

private:
    boost::beast::multi_buffer m_buffer;
    websocket_t m_socket;
};


template <typename Self>
class application_endpoint_base<Self, tcp::socket>
    : public std::enable_shared_from_this<application_endpoint_base<Self, tcp::socket>> {
public:
    application_endpoint_base(boost::asio::io_context& ctx)
        : m_ctx(ctx), m_strand(ctx.get_executor()), m_socket(tcp::socket(ctx)) {}
    template <typename CB>
    void read_message(CB handle) {
        auto that = this->shared_from_this();
        auto cb = boost::asio::bind_executor(
            m_strand, [that, handle = std::move(handle)](boost::system::error_code ec, fb_message_ptr&& msg) {
                handle(ec, std::move(msg));
            });
        mobsya::async_read_flatbuffers_message(m_socket, std::move(cb));
    }

    void do_write_message(const flatbuffers::DetachedBuffer& buffer) {
        auto cb = boost::asio::bind_executor(m_strand, [that = this->shared_from_this()](boost::system::error_code ec) {
            static_cast<Self&>(*that).handle_write(ec);
        });
        mobsya::async_write_flatbuffer_message(m_socket, buffer, std::move(cb));
    }

    void start() {
        static_cast<Self*>(this)->on_initialized();
    }

    tcp::socket& tcp_socket() {
        return m_socket;
    }

protected:
    boost::asio::io_context& m_ctx;
    boost::asio::strand<boost::asio::io_context::executor_type> m_strand;

private:
    tcp::socket m_socket;
};

template <typename Socket>
class application_endpoint : public application_endpoint_base<application_endpoint<Socket>, Socket>,
                             public node_status_monitor {
public:
    using base = application_endpoint_base<application_endpoint<Socket>, Socket>;
    application_endpoint(boost::asio::io_context& ctx) : base(ctx), m_ctx(ctx) {}

    void set_local(bool is_local) {
        this->m_local_endpoint = is_local;
    }

    void start() {
        mLogInfo("Starting app endpoint");
        base::start();
    }

    void on_initialized(boost::system::error_code ec = {}) {
        mLogTrace("on_initialized: {}", ec.message());

        // start listening for incomming messages
        read_message(
            [this](boost::system::error_code ec, fb_message_ptr&& msg) { this->handle_handshake(ec, std::move(msg)); });

        // Subscribe to node change events
        start_node_monitoring(registery());
    }

    template <typename CB>
    void read_message(CB&& handle) {
        base::read_message(std::forward<CB>(handle));
    }

    void read_message() {
        read_message(
            [this](boost::system::error_code ec, fb_message_ptr&& msg) { this->handle_read(ec, std::move(msg)); });
    }

    void write_message(tagged_detached_flatbuffer&& buffer) {
        m_queue.emplace(std::move(buffer));
        if(m_queue.size() > 1 || m_protocol_version == 0)
            return;

        base::do_write_message(m_queue.front().buffer);
    }


    void handle_read(boost::system::error_code ec, fb_message_ptr&& msg) {
        if(ec) {
            mLogError("Network error while reading TDM message {}", ec.message());
            return;
        }
        read_message();  // queue the next read early


        mLogTrace("-> {}", EnumNameAnyMessage(msg.message_type()));
        switch(msg.message_type()) {
            case mobsya::fb::AnyMessage::RequestListOfNodes: send_full_node_list(); break;
            case mobsya::fb::AnyMessage::RequestNodeAsebaVMDescription: {
                auto req = msg.as<fb::RequestNodeAsebaVMDescription>();
                send_aseba_vm_description(req->request_id(), req->node_id());
                break;
            }
            case mobsya::fb::AnyMessage::SetNodeVariables: {
                auto vars_msg = msg.as<fb::SetNodeVariables>();
                this->set_node_variables(vars_msg->request_id(), vars_msg->node_id(), variables(*vars_msg));
                break;
            }
            case mobsya::fb::AnyMessage::RegisterEvents: {
                auto vars_msg = msg.as<fb::RegisterEvents>();
                this->set_node_events_table(vars_msg->request_id(), vars_msg->node_id(), events_description(*vars_msg));
                break;
            }
            case mobsya::fb::AnyMessage::SendEvents: {
                auto vars_msg = msg.as<fb::SendEvents>();
                this->emit_events(vars_msg->request_id(), vars_msg->node_id(), events(*vars_msg));
                break;
            }
            case mobsya::fb::AnyMessage::RenameNode: {
                auto rename_msg = msg.as<fb::RenameNode>();
                this->rename_node(rename_msg->request_id(), rename_msg->node_id(), rename_msg->new_name()->str());
                break;
            }
            case mobsya::fb::AnyMessage::LockNode: {
                auto lock_msg = msg.as<fb::LockNode>();
                this->lock_node(lock_msg->request_id(), lock_msg->node_id());
                break;
            }
            case mobsya::fb::AnyMessage::UnlockNode: {
                auto lock_msg = msg.as<fb::UnlockNode>();
                this->unlock_node(lock_msg->request_id(), lock_msg->node_id());
                break;
            }
            case mobsya::fb::AnyMessage::CompileAndLoadCodeOnVM: {
                auto req = msg.as<fb::CompileAndLoadCodeOnVM>();
                this->compile_and_send_program(req->request_id(), req->node_id(), vm_language(req->language()),
                                               req->program()->str(), req->options());
                break;
            }
            case mobsya::fb::AnyMessage::SetVMExecutionState: {
                auto req = msg.as<fb::SetVMExecutionState>();
                this->set_vm_execution_state(req->request_id(), req->node_id(), req->command());
                break;
            }
            case mobsya::fb::AnyMessage::WatchNode: {
                auto req = msg.as<fb::WatchNode>();
                this->watch_node(req->request_id(), req->node_id(), req->info_type());
                break;
            }
            case mobsya::fb::AnyMessage::SetBreakpoints: {
                auto req = msg.as<fb::SetBreakpoints>();
                this->set_breakpoints(req->request_id(), req->node_id(), breakpoints(*req));
                break;
            }


            default: mLogWarn("Message {} from application unsupported", EnumNameAnyMessage(msg.message_type())); break;
        }
    }

    void handle_write(boost::system::error_code ec) {
        mLogTrace("<- {} : {} ", EnumNameAnyMessage(m_queue.front().tag), ec.message());
        if(ec) {
            mLogError("handle_write : error {}", ec.message());
        }
        m_queue.pop();
        if(!m_queue.empty()) {
            base::do_write_message(m_queue.front().buffer);
        }
    }

    ~application_endpoint() {
        mLogInfo("Stopping app endpoint");

        /* Disconnecting the node monotoring status before unlocking the nodes,
         * otherwise we would receive node status event during destroying the endpoint, leading to a crash */
        disconnect();

        for(auto& p : m_locked_nodes) {
            auto ptr = p.second.lock();
            if(ptr) {
                ptr->unlock(this);
            }
        }
    }

    void node_changed(std::shared_ptr<aseba_node> node, const aseba_node_registery::node_id& id,
                      aseba_node::status status) {
        boost::asio::post(this->m_strand, [that = this->shared_from_this(), node, id, status]() {
            that->do_node_changed(node, id, status);
        });
    }

    void node_variables_changed(std::shared_ptr<aseba_node> node, const aseba_node::variables_map& map) {
        boost::asio::defer(this->m_strand, [that = this->shared_from_this(), node, map]() {
            that->do_node_variables_changed(node, map);
        });
    }

    void node_emitted_events(std::shared_ptr<aseba_node> node, const aseba_node::event_changed_payload& payload) {
        boost::asio::defer(this->m_strand, [that = this->shared_from_this(), node, payload]() {
            that->do_node_emitted_events(node, payload);
        });
    }

    void node_execution_state_changed(std::shared_ptr<aseba_node> node, const aseba_node::vm_execution_state& state) {
        boost::asio::defer(this->m_strand, [that = this->shared_from_this(), node, state]() {
            that->do_node_execution_state_changed(node, state);
        });
    }

private:
    void do_node_changed(std::shared_ptr<aseba_node> node, const aseba_node_registery::node_id& id,
                         aseba_node::status status) {
        // mLogInfo("node changed: {}, {}", node->native_id(), node->status_to_string(status));

        if(status == aseba_node::status::busy && get_locked_node(id)) {
            status = aseba_node::status::ready;
        }

        flatbuffers::FlatBufferBuilder builder;
        std::vector<flatbuffers::Offset<fb::Node>> nodes;
        nodes.emplace_back(fb::CreateNodeDirect(builder, id.fb(builder), mobsya::fb::NodeStatus(status), node->type(),
                                                node->friendly_name().c_str(), node_capabilities(node)));
        auto vector_offset = builder.CreateVector(nodes);
        auto offset = CreateNodesChanged(builder, vector_offset);
        write_message(wrap_fb(builder, offset));

        if(status == aseba_node::status::disconnected) {
            m_locked_nodes.erase(id);
        }
    }

    void do_node_variables_changed(std::shared_ptr<aseba_node> node, const aseba_node::variables_map& map) {
        if(!node)
            return;
        write_message(serialize_changed_variables(*node, map));
    }

    void do_node_emitted_events(std::shared_ptr<aseba_node> node, const aseba_node::event_changed_payload& payload) {
        if(!node)
            return;
        variant_ns::visit(overloaded{[this, &node](const aseba_node::variables_map& map) {
                                         write_message(serialize_events(*node, map));
                                     },
                                     [this, &node](const aseba_node::events_table& desc) {
                                         write_message(serialize_events_descriptions(*node, desc));
                                     }},
                          payload);
    }

    void do_node_execution_state_changed(std::shared_ptr<aseba_node> node,
                                         const aseba_node::vm_execution_state& state) {
        if(!node)
            return;
        write_message(serialize_execution_state(*node, state));
    }


    void send_full_node_list() {
        flatbuffers::FlatBufferBuilder builder;
        std::vector<flatbuffers::Offset<fb::Node>> nodes;
        auto map = registery().nodes();
        for(auto& node : map) {
            const auto ptr = node.second.lock();
            if(!ptr)
                continue;
            nodes.emplace_back(fb::CreateNodeDirect(builder, node.first.fb(builder),
                                                    mobsya::fb::NodeStatus(ptr->get_status()), ptr->type(),
                                                    ptr->friendly_name().c_str(), node_capabilities(ptr)));
        }
        auto vector_offset = builder.CreateVector(nodes);
        auto offset = CreateNodesChanged(builder, vector_offset);
        write_message(wrap_fb(builder, offset));
    }

    uint64_t node_capabilities(std::shared_ptr<aseba_node> node) const {
        uint64_t caps = 0;
        if(m_local_endpoint) {
            caps |= uint64_t(fb::NodeCapability::ForceResetAndStop);
            if(node->can_be_renamed())
                caps |= uint64_t(fb::NodeCapability::Rename);
        }
        return caps;
    }

    void send_aseba_vm_description(uint32_t request_id, const aseba_node_registery::node_id& id) {
        auto node = registery().node_from_id(id);
        if(!node) {
            // error ?
            return;
        }
        write_message(serialize_aseba_vm_description(request_id, *node, id));
    }

    void rename_node(uint32_t request_id, const aseba_node_registery::node_id& id, const std::string& new_name) {
        auto n = registery().node_from_id(id);
        if(!n || !(node_capabilities(n) & uint64_t(fb::NodeCapability::Rename))) {
            mLogWarn("rename_node: node {} does not exist or can not be renamed", id);
            write_message(create_error_response(request_id, fb::ErrorType::unknown_node));
            return;
        }
        n->rename(new_name);
        write_message(create_ack_response(request_id));
    }

    void lock_node(uint32_t request_id, const aseba_node_registery::node_id& id) {
        auto node = registery().node_from_id(id);
        if(!node) {
            write_message(create_error_response(request_id, fb::ErrorType::unknown_node));
            return;
        }
        m_locked_nodes[id] = node;
        bool res = node->lock(this);
        if(!res) {
            m_locked_nodes.erase(id);
            write_message(create_error_response(request_id, fb::ErrorType::node_busy));
        } else {
            write_message(create_ack_response(request_id));
        }
    }

    void unlock_node(uint32_t request_id, const aseba_node_registery::node_id& id) {
        auto it = m_locked_nodes.find(id);
        std::shared_ptr<aseba_node> node;
        if(it != std::end(m_locked_nodes)) {
            node = it->second.lock();
            m_locked_nodes.erase(it);
        }

        if(!node) {
            write_message(create_error_response(request_id, fb::ErrorType::unknown_node));
            return;
        }
        if(!node->unlock(this)) {
            write_message(create_error_response(request_id, fb::ErrorType::node_busy));
        } else {
            write_message(create_ack_response(request_id));
        }
    }

    void set_node_variables(uint32_t request_id, const aseba_node_registery::node_id& id, aseba_node::variables_map m) {
        auto n = get_locked_node(id);
        if(!n) {
            mLogWarn("set_node_variables: node {} not locked", id);
            write_message(create_error_response(request_id, fb::ErrorType::unknown_node));
            return;
        }
        auto err = n->set_node_variables(m, create_device_write_completion_cb(request_id));
        if(err) {
            mLogWarn("set_node_variables: invalid variables", id);
            write_message(create_error_response(request_id, fb::ErrorType::unsupported_variable_type));
        }
    }

    void set_node_events_table(uint32_t request_id, const aseba_node_registery::node_id& id,
                               aseba_node::events_table events) {
        auto n = get_locked_node(id);
        if(!n) {
            mLogWarn("set_node_events_table: node {} not locked", id);
            write_message(create_error_response(request_id, fb::ErrorType::unknown_node));
            return;
        }
        auto err = n->set_node_events_table(events);
        if(err) {
            mLogWarn("set_node_events_table: invalid events", id);
            write_message(create_error_response(request_id, fb::ErrorType::unsupported_variable_type));
        } else {
            write_message(create_ack_response(request_id));
        }
    }

    void emit_events(uint32_t request_id, const aseba_node_registery::node_id& id, aseba_node::variables_map m) {
        auto n = get_locked_node(id);
        if(!n) {
            mLogWarn("emits_events: node {} not locked", id);
            write_message(create_error_response(request_id, fb::ErrorType::unknown_node));
            return;
        }
        auto err = n->emit_events(m, create_device_write_completion_cb(request_id));
        if(err) {
            mLogWarn("emits_events: invalid variables", id);
            write_message(create_error_response(request_id, fb::ErrorType::unsupported_variable_type));
        }
    }

    void compile_and_send_program(uint32_t request_id, const aseba_node_registery::node_id& id, vm_language language,
                                  std::string program, fb::CompilationOptions opts) {
        auto n = get_locked_node(id);
        if(!n) {
            mLogWarn("send_aseba_code: node {} not locked", id);
            write_message(create_error_response(request_id, fb::ErrorType::unknown_node));
            return;
        }
        auto callback = [request_id, strand = this->m_strand,
                         ptr = weak_from_this()](boost::system::error_code ec, aseba_node::compilation_result result) {
            boost::asio::post(strand, [ec, result, request_id, ptr]() {
                auto that = ptr.lock();
                if(!that)
                    return;
                if(ec) {
                    that->write_message(create_error_response(request_id, fb::ErrorType::unknown_node));
                    return;
                }
                that->write_message(create_compilation_result_response(request_id, result));
            });
        };
        if((int32_t(opts) & int32_t(fb::CompilationOptions::LoadOnTarget))) {
            n->compile_and_send_program(language, program, callback);
        } else {
            n->compile_program(language, program, callback);
        }
    }

    void set_vm_execution_state(uint32_t request_id, aseba_node_registery::node_id id,
                                fb::VMExecutionStateCommand cmd) {
        auto n = get_locked_node(id);
        if(!n && cmd == fb::VMExecutionStateCommand::Stop) {
            n = registery().node_from_id(id);
            if(n && (node_capabilities(n) & uint64_t(fb::NodeCapability::Rename)))
                n = {};
        }
        if(!n) {
            mLogWarn("set_vm_execution_state: node {} not locked", id);
            write_message(create_error_response(request_id, fb::ErrorType::unknown_node));
            return;
        }
        n->set_vm_execution_state(cmd, create_device_write_completion_cb(request_id));
    }

    void set_breakpoints(uint32_t request_id, aseba_node_registery::node_id id, std::vector<breakpoint> breakpoints) {
        auto n = get_locked_node(id);
        if(!n) {
            mLogWarn("set_breakpoints: node {} not locked", id);
            write_message(create_error_response(request_id, fb::ErrorType::unknown_node));
            return;
        }
        auto callback = [request_id, strand = this->m_strand, ptr = weak_from_this()](boost::system::error_code ec,
                                                                                      aseba_node::breakpoints bps) {
            boost::asio::post(strand, [ec, bps, request_id, ptr]() {
                auto that = ptr.lock();
                if(!that)
                    return;
                that->write_message(create_set_breakpoint_response(
                    request_id, ec ? fb::ErrorType::unknown_error : fb::ErrorType::no_error, bps));
            });
        };

        n->set_breakpoints(breakpoints, callback);
    }

    void watch_node(uint32_t request_id, const aseba_node_registery::node_id& id, uint32_t flags) {
        auto node = registery().node_from_id(id);
        if(!node) {
            write_message(create_error_response(request_id, fb::ErrorType::unknown_node));
            return;
        }
        if(flags & uint32_t(fb::WatchableInfo::Variables)) {
            if(!m_watch_nodes[fb::WatchableInfo::Variables].count(id)) {
                auto variables = node->variables();
                this->node_variables_changed(node, variables);
            }
            m_watch_nodes[fb::WatchableInfo::Variables][id] = node->connect_to_variables_changes(std::bind(
                &application_endpoint::node_variables_changed, this, std::placeholders::_1, std::placeholders::_2));
        } else {
            m_watch_nodes[fb::WatchableInfo::Variables].erase(id);
        }

        if(flags & uint32_t(fb::WatchableInfo::Events)) {
            m_watch_nodes[fb::WatchableInfo::Events][id] = node->connect_to_events(std::bind(
                &application_endpoint::node_emitted_events, this, std::placeholders::_1, std::placeholders::_2));
            this->node_emitted_events(node, node->events_description());
        } else {
            m_watch_nodes[fb::WatchableInfo::Events].erase(id);
        }

        if(flags & uint32_t(fb::WatchableInfo::VMExecutionState)) {
            m_watch_nodes[fb::WatchableInfo::VMExecutionState][id] =
                node->connect_to_execution_state_changes(std::bind(&application_endpoint::node_execution_state_changed,
                                                                   this, std::placeholders::_1, std::placeholders::_2));
            this->node_execution_state_changed(node, node->execution_state());

        } else {
            m_watch_nodes[fb::WatchableInfo::VMExecutionState].erase(id);
        }


        write_message(create_ack_response(request_id));
    }

    aseba_node_registery& registery() {
        return boost::asio::use_service<aseba_node_registery>(this->m_ctx);
    }

    std::shared_ptr<aseba_node> get_locked_node(const aseba_node_registery::node_id& id) const {
        auto it = m_locked_nodes.find(id);
        if(it == std::end(m_locked_nodes))
            return {};
        return it->second.lock();
    }

    /*
     *  Returns a std::function that, when called posts a lambda in the endpoint strand
     *  Said lambda is ultimately responsible for sending the ack message to the app,
     *  if it still exists.
     */
    aseba_node::write_callback create_device_write_completion_cb(uint32_t request_id) {
        auto strand = this->m_strand;
        auto ptr = weak_from_this();
        auto callback = [request_id, strand, ptr](boost::system::error_code ec) {
            boost::asio::post(strand, [ec, request_id, ptr]() {
                auto that = ptr.lock();
                if(!that)
                    return;
                if(!ec) {
                    that->write_message(create_ack_response(request_id));
                } else {
                    that->write_message(create_error_response(request_id, fb::ErrorType::node_busy));
                }
            });
        };
        return callback;
    }

    void handle_handshake(boost::system::error_code ec, fb_message_ptr&& msg) {
        if(ec) {
            mLogError("Network error while reading TDM message {}", ec.message());
            return;
        }

        if(msg.message_type() != mobsya::fb::AnyMessage::ConnectionHandshake) {
            mLogError("Client did not send a ConnectionHandshake message");
            return;
        }
        auto hs = msg.as<fb::ConnectionHandshake>();
        if(hs->protocolVersion() < tdm::minProtocolVersion || tdm::protocolVersion < hs->minProtocolVersion()) {
            mLogError("Client protocol version ({}) is not compatible with this server({}+)", hs->protocolVersion(),
                      tdm::minProtocolVersion);
        } else {
            m_protocol_version = std::min(hs->protocolVersion(), tdm::protocolVersion);
            m_max_out_going_packet_size = hs->maxMessageSize();
            auto& token_manager = boost::asio::use_service<app_token_manager>(m_ctx);
            // TODO ?
            if(hs->token())
                token_manager.check_token(app_token_manager::token_view{hs->token()->data(), hs->token()->size()});
        }
        flatbuffers::FlatBufferBuilder builder;
        write_message(wrap_fb(builder,
                              fb::CreateConnectionHandshake(builder, tdm::minProtocolVersion, m_protocol_version,
                                                            tdm::maxAppEndPointMessageSize)));

        // the client do not have a compatible protocol version, bailing out
        if(m_protocol_version == 0) {
            return;
        }

        // Once the handshake is complete, send a list of nodes, that will also flush out all pending outgoing
        // messages
        send_full_node_list();

        read_message();
    }

    std::shared_ptr<application_endpoint<Socket>> shared_from_this() {
        return std::static_pointer_cast<application_endpoint<Socket>>(base::shared_from_this());
    }

    std::weak_ptr<application_endpoint<Socket>> weak_from_this() {
        return std::static_pointer_cast<application_endpoint<Socket>>(this->shared_from_this());
    }

    boost::asio::io_context& m_ctx;
    std::queue<tagged_detached_flatbuffer> m_queue;
    std::unordered_map<aseba_node_registery::node_id, std::weak_ptr<aseba_node>, boost::hash<boost::uuids::uuid>>
        m_locked_nodes;
    std::unordered_map<fb::WatchableInfo,
                       std::unordered_map<aseba_node_registery::node_id, boost::signals2::scoped_connection>>
        m_watch_nodes;
    uint16_t m_protocol_version = 0;
    uint16_t m_max_out_going_packet_size = 0;
    bool m_local_endpoint = false;
};  // namespace mobsya

}  // namespace mobsya
