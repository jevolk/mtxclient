#include "mtxclient/http/client.hpp"

#include <mutex>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/bind.hpp>
#include <boost/utility/typed_in_place_factory.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/signals2.hpp>
#include <boost/signals2/signal_type.hpp>
#include <boost/thread/thread.hpp>

#include "mtxclient/http/session.hpp"
#include "mtxclient/utils.hpp"

#include "mtx/requests.hpp"
#include "mtx/responses.hpp"

using namespace mtx::http;
using namespace boost::beast;

namespace mtx::http {
struct ClientPrivate
{
        boost::asio::io_service ios_;
        //! Used to prevent the event loop from shutting down.
        std::optional<boost::asio::io_context::work> work_{ios_};
        //! Worker threads for the requests.
        boost::thread_group thread_group_;
        //! SSL context for requests.
        boost::asio::ssl::context ssl_ctx_{boost::asio::ssl::context::sslv23_client};
        //! All the active sessions will shutdown the connection.
        boost::signals2::signal<void()> shutdown_signal;
};
}

Client::Client(const std::string &server, uint16_t port)
  : server_{server}
  , port_{port}
  , p{new ClientPrivate}
{
        using namespace boost::asio;
        const auto threads_num = std::max(1U, std::thread::hardware_concurrency());

        for (unsigned int i = 0; i < threads_num; ++i)
                p->thread_group_.add_thread(new boost::thread([this]() { p->ios_.run(); }));
}

// call destuctor of work queue and ios first!
Client::~Client() { p.reset(); }

std::shared_ptr<Session>
Client::create_session(TypeErasedCallback type_erased_cb)
{
        auto session = std::make_shared<Session>(
          std::ref(p->ios_),
          std::ref(p->ssl_ctx_),
          server_,
          port_,
          client::utils::random_token(),
          [type_erased_cb](
            RequestID,
            const boost::beast::http::response<boost::beast::http::string_body> &response,
            const boost::system::error_code &err_code) {
                  const auto header = response.base();

                  if (err_code) {
                          return type_erased_cb(header, "", err_code, {});
                  }

                  // Decompress the response.
                  const auto body = client::utils::decompress(
                    boost::iostreams::array_source{response.body().data(), response.body().size()},
                    header["Content-Encoding"].to_string());

                  type_erased_cb(header, body, err_code, response.result());
          },
          [type_erased_cb](RequestID, const boost::system::error_code ec) {
                  type_erased_cb(std::nullopt, "", ec, {});
          });
        if (session)
                p->shutdown_signal.connect(
                  boost::signals2::signal<void()>::slot_type(&Session::terminate, session.get())
                    .track_foreign(session));

        return session;
}

void
Client::shutdown()
{
        p->shutdown_signal();
}

void
mtx::http::Client::post(const std::string &endpoint,
                        const nlohmann::json &req,
                        mtx::http::TypeErasedCallback cb,
                        bool requires_auth,
                        const std::string &content_type)
{
        auto session = create_session(cb);

        if (!session)
                return;

        setup_auth(session.get(), requires_auth);
        setup_headers<boost::beast::http::verb::post>(session.get(), req, endpoint, content_type);

        session->run();
}

void
mtx::http::Client::delete_(const std::string &endpoint, ErrCallback cb, bool requires_auth)
{
        auto type_erased_cb = [cb](HeaderFields,
                                   const std::string &body,
                                   const boost::system::error_code &err_code,
                                   boost::beast::http::status status_code) {
                mtx::http::ClientError client_error;

                if (err_code) {
                        client_error.error_code = err_code;
                        return cb(client_error);
                }

                client_error.status_code = status_code;

                // We only count 2xx status codes as success.
                if (static_cast<int>(status_code) < 200 || static_cast<int>(status_code) >= 300) {
                        // The homeserver should return an error struct.
                        try {
                                nlohmann::json json_error       = json::parse(body);
                                mtx::errors::Error matrix_error = json_error;

                                client_error.matrix_error = matrix_error;
                        } catch (const nlohmann::json::exception &e) {
                                client_error.parse_error = std::string(e.what()) + ": " + body;
                        }
                        return cb(client_error);
                }
                return cb({});
        };

        auto session = create_session(type_erased_cb);

        if (!session)
                return;

        setup_auth(session.get(), requires_auth);
        setup_headers<boost::beast::http::verb::delete_>(session.get(), "", endpoint, "");

        session->run();
}

void
mtx::http::Client::put(const std::string &endpoint,
                       const nlohmann::json &req,
                       mtx::http::TypeErasedCallback cb,
                       bool requires_auth)
{
        auto session = create_session(cb);

        if (!session)
                return;

        setup_auth(session.get(), requires_auth);
        setup_headers<boost::beast::http::verb::put>(
          session.get(), req, endpoint, "application/json");

        session->run();
}

void
mtx::http::Client::get(const std::string &endpoint,
                       mtx::http::TypeErasedCallback cb,
                       bool requires_auth,
                       const std::string &endpoint_namespace)
{
        auto session = create_session(cb);

        if (!session)
                return;

        setup_auth(session.get(), requires_auth);
        setup_headers<boost::beast::http::verb::get>(
          session.get(), client::utils::serialize(std::string{}), endpoint, "", endpoint_namespace);

        session->run();
}

void
Client::set_server(const std::string &server)
{
        std::string server_name = server;
        // Remove https prefix, if it exists
        if (boost::algorithm::starts_with(server_name, "https://"))
                boost::algorithm::erase_first(server_name, "https://");
        if (server_name.size() > 0 && server_name.back() == '/')
                server_name.erase(server_name.end() - 1);

        // Check if the input also contains the port.
        std::vector<std::string> parts;
        boost::split(parts, server_name, [](char c) { return c == ':'; });

        if (parts.size() == 2 && mtx::client::utils::is_number(parts.at(1))) {
                server_ = parts.at(0);
                port_   = std::stoi(parts.at(1));
        } else {
                server_ = server_name;
        }
}

void
Client::close(bool force)
{
        // We close all open connections.
        if (force) {
                shutdown();
                p->ios_.stop();
        }

        // Destroy work object. This allows the I/O thread to
        // exit the event loop when there are no more pending
        // asynchronous operations.
        p->work_.reset();

        // Wait for the worker threads to exit.
        p->thread_group_.join_all();
}

void
Client::setup_auth(Session *session, bool auth)
{
        const auto token = access_token();

        if (auth && !token.empty())
                session->request.set(boost::beast::http::field::authorization, "Bearer " + token);
}

//
// Client API endpoints
//

void
Client::login(const std::string &user,
              const std::string &password,
              const std::string &device_name,
              Callback<mtx::responses::Login> callback)
{
        mtx::requests::Login req;
        req.user                        = user;
        req.password                    = password;
        req.initial_device_display_name = device_name;

        login(req, callback);
}

void
Client::login(const std::string &user,
              const std::string &password,
              Callback<mtx::responses::Login> callback)
{
        mtx::requests::Login req;
        req.user     = user;
        req.password = password;

        login(req, callback);
}

void
Client::login(const mtx::requests::Login &req, Callback<mtx::responses::Login> callback)
{
        post<mtx::requests::Login, mtx::responses::Login>(
          "/client/r0/login",
          req,
          [_this = shared_from_this(), callback](const mtx::responses::Login &resp,
                                                 RequestErr err) {
                  if (!err && resp.access_token.size()) {
                          _this->user_id_      = resp.user_id;
                          _this->device_id_    = resp.device_id;
                          _this->access_token_ = resp.access_token;
                  }
                  callback(resp, err);
          },
          false);
}

void
Client::well_known(Callback<mtx::responses::WellKnown> callback)
{
        get<mtx::responses::WellKnown>(
          "/matrix/client",
          [callback](const mtx::responses::WellKnown &res, HeaderFields, RequestErr err) {
                  callback(res, err);
          },
          false,
          "/.well-known");
}

void
Client::logout(Callback<mtx::responses::Logout> callback)
{
        mtx::requests::Logout req;

        post<mtx::requests::Logout, mtx::responses::Logout>(
          "/client/r0/logout",
          req,
          [_this = shared_from_this(), callback](const mtx::responses::Logout &res,
                                                 RequestErr err) {
                  if (!err) {
                          // Clear the now invalid access token when logout is successful
                          _this->access_token_.clear();
                  }
                  // Pass up response and error to supplied callback
                  callback(res, err);
          });
}

void
Client::notifications(uint64_t limit,
                      const std::string &from,
                      const std::string &only,
                      Callback<mtx::responses::Notifications> cb)
{
        std::map<std::string, std::string> params;
        params.emplace("limit", std::to_string(limit));

        if (!from.empty()) {
                params.emplace("from", from);
        }

        if (!only.empty()) {
                params.emplace("only", only);
        }

        get<mtx::responses::Notifications>(
          "/client/r0/notifications?" + mtx::client::utils::query_params(params),
          [cb](const mtx::responses::Notifications &res, HeaderFields, RequestErr err) {
                  cb(res, err);
          });
}

void
Client::get_pushrules(Callback<mtx::pushrules::GlobalRuleset> cb)
{
        get<mtx::pushrules::GlobalRuleset>("/client/r0/pushrules/",
                                           [cb](const mtx::pushrules::GlobalRuleset &res,
                                                HeaderFields,
                                                RequestErr err) { cb(res, err); });
}

void
Client::get_pushrules(const std::string &scope,
                      const std::string &kind,
                      const std::string &ruleId,
                      Callback<mtx::pushrules::PushRule> cb)
{
        get<mtx::pushrules::PushRule>("/client/r0/pushrules/" + scope + "/" + kind + "/" + ruleId,
                                      [cb](const mtx::pushrules::PushRule &res,
                                           HeaderFields,
                                           RequestErr err) { cb(res, err); });
}

void
Client::delete_pushrules(const std::string &scope,
                         const std::string &kind,
                         const std::string &ruleId,
                         ErrCallback cb)
{
        delete_("/client/r0/pushrules/" + scope + "/" + kind + "/" +
                  mtx::client::utils::url_encode(ruleId),
                cb);
}

void
Client::put_pushrules(const std::string &scope,
                      const std::string &kind,
                      const std::string &ruleId,
                      const mtx::pushrules::PushRule &rule,
                      ErrCallback cb,
                      const std::string &before,
                      const std::string &after)
{
        std::map<std::string, std::string> params;

        if (!before.empty())
                params.emplace("before", before);

        if (!after.empty())
                params.emplace("after", after);

        put<mtx::pushrules::PushRule>("/client/r0/pushrules/" + scope + "/" + kind + "/" +
                                        mtx::client::utils::url_encode(ruleId) + "?" +
                                        mtx::client::utils::query_params(params),
                                      rule,
                                      cb);
}

void
Client::get_pushrules_enabled(const std::string &scope,
                              const std::string &kind,
                              const std::string &ruleId,
                              Callback<mtx::pushrules::Enabled> cb)
{
        get<mtx::pushrules::Enabled>(
          "/client/r0/pushrules/" + scope + "/" + kind + "/" +
            mtx::client::utils::url_encode(ruleId) + "/enabled",
          [cb](const mtx::pushrules::Enabled &res, HeaderFields, RequestErr err) { cb(res, err); });
}

void
Client::put_pushrules_enabled(const std::string &scope,
                              const std::string &kind,
                              const std::string &ruleId,
                              bool enabled,
                              ErrCallback cb)
{
        put<mtx::pushrules::Enabled>("/client/r0/pushrules/" + scope + "/" + kind + "/" +
                                       mtx::client::utils::url_encode(ruleId) + "/enabled",
                                     {enabled},
                                     cb);
}

void
Client::get_pushrules_actions(const std::string &scope,
                              const std::string &kind,
                              const std::string &ruleId,
                              Callback<mtx::pushrules::actions::Actions> cb)
{
        get<mtx::pushrules::actions::Actions>("/client/r0/pushrules/" + scope + "/" + kind + "/" +
                                                mtx::client::utils::url_encode(ruleId) + "/actions",
                                              [cb](const mtx::pushrules::actions::Actions &res,
                                                   HeaderFields,
                                                   RequestErr err) { cb(res, err); });
}

void
Client::put_pushrules_actions(const std::string &scope,
                              const std::string &kind,
                              const std::string &ruleId,
                              const mtx::pushrules::actions::Actions &actions,
                              ErrCallback cb)
{
        put<mtx::pushrules::actions::Actions>("/client/r0/pushrules/" + scope + "/" + kind + "/" +
                                                mtx::client::utils::url_encode(ruleId) + "/actions",
                                              actions,
                                              cb);
}

void
Client::set_avatar_url(const std::string &avatar_url, ErrCallback callback)
{
        mtx::requests::AvatarUrl req;
        req.avatar_url = avatar_url;

        put<mtx::requests::AvatarUrl>("/client/r0/profile/" +
                                        mtx::client::utils::url_encode(user_id_.to_string()) +
                                        "/avatar_url",
                                      req,
                                      callback);
}

void
Client::set_displayname(const std::string &displayname, ErrCallback callback)
{
        mtx::requests::DisplayName req;
        req.displayname = displayname;

        put<mtx::requests::DisplayName>("/client/r0/profile/" +
                                          mtx::client::utils::url_encode(user_id_.to_string()) +
                                          "/displayname",
                                        req,
                                        callback);
}

void
Client::get_profile(const std::string &user_id, Callback<mtx::responses::Profile> callback)
{
        get<mtx::responses::Profile>(
          "/client/r0/profile/" + mtx::client::utils::url_encode(user_id),
          [callback](const mtx::responses::Profile &res, HeaderFields, RequestErr err) {
                  callback(res, err);
          });
}

void
Client::get_avatar_url(const std::string &user_id, Callback<mtx::responses::AvatarUrl> callback)
{
        get<mtx::responses::AvatarUrl>(
          "/client/r0/profile/" + mtx::client::utils::url_encode(user_id) + "/avatar_url",
          [callback](const mtx::responses::AvatarUrl &res, HeaderFields, RequestErr err) {
                  callback(res, err);
          });
}

void
Client::create_room(const mtx::requests::CreateRoom &room_options,
                    Callback<mtx::responses::CreateRoom> callback)
{
        post<mtx::requests::CreateRoom, mtx::responses::CreateRoom>(
          "/client/r0/createRoom", room_options, callback);
}

void
Client::join_room(const std::string &room, Callback<nlohmann::json> callback)
{
        auto api_path = "/client/r0/join/" + mtx::client::utils::url_encode(room);

        post<std::string, nlohmann::json>(api_path, "", callback);
}

void
Client::leave_room(const std::string &room_id, Callback<nlohmann::json> callback)
{
        auto api_path = "/client/r0/rooms/" + mtx::client::utils::url_encode(room_id) + "/leave";

        post<std::string, nlohmann::json>(api_path, "", callback);
}

void
Client::invite_user(const std::string &room_id,
                    const std::string &user_id,
                    Callback<mtx::responses::RoomInvite> callback,
                    const std::string &reason)
{
        mtx::requests::RoomMembershipChange req;
        req.user_id = user_id;
        req.reason  = reason;

        auto api_path = "/client/r0/rooms/" + mtx::client::utils::url_encode(room_id) + "/invite";

        post<mtx::requests::RoomMembershipChange, mtx::responses::RoomInvite>(
          api_path, req, callback);
}

void
Client::kick_user(const std::string &room_id,
                  const std::string &user_id,
                  Callback<mtx::responses::Empty> callback,
                  const std::string &reason)
{
        mtx::requests::RoomMembershipChange req;
        req.user_id = user_id;
        req.reason  = reason;

        auto api_path = "/client/r0/rooms/" + mtx::client::utils::url_encode(room_id) + "/kick";

        post<mtx::requests::RoomMembershipChange, mtx::responses::Empty>(api_path, req, callback);
}

void
Client::ban_user(const std::string &room_id,
                 const std::string &user_id,
                 Callback<mtx::responses::Empty> callback,
                 const std::string &reason)
{
        mtx::requests::RoomMembershipChange req;
        req.user_id = user_id;
        req.reason  = reason;

        auto api_path = "/client/r0/rooms/" + mtx::client::utils::url_encode(room_id) + "/ban";

        post<mtx::requests::RoomMembershipChange, mtx::responses::Empty>(api_path, req, callback);
}

void
Client::unban_user(const std::string &room_id,
                   const std::string &user_id,
                   Callback<mtx::responses::Empty> callback,
                   const std::string &reason)
{
        mtx::requests::RoomMembershipChange req;
        req.user_id = user_id;
        req.reason  = reason;

        auto api_path = "/client/r0/rooms/" + mtx::client::utils::url_encode(room_id) + "/unban";

        post<mtx::requests::RoomMembershipChange, mtx::responses::Empty>(api_path, req, callback);
}

void
Client::sync(const SyncOpts &opts, Callback<mtx::responses::Sync> callback)
{
        std::map<std::string, std::string> params;

        if (!opts.filter.empty())
                params.emplace("filter", opts.filter);

        if (!opts.since.empty())
                params.emplace("since", opts.since);

        if (opts.full_state)
                params.emplace("full_state", "true");

        params.emplace("timeout", std::to_string(opts.timeout));

        get<mtx::responses::Sync>("/client/r0/sync?" + mtx::client::utils::query_params(params),
                                  [callback](const mtx::responses::Sync &res,
                                             HeaderFields,
                                             RequestErr err) { callback(res, err); });
}

void
Client::versions(Callback<mtx::responses::Versions> callback)
{
        get<mtx::responses::Versions>("/client/versions",
                                      [callback](const mtx::responses::Versions &res,
                                                 HeaderFields,
                                                 RequestErr err) { callback(res, err); });
}

void
Client::upload(const std::string &data,
               const std::string &content_type,
               const std::string &filename,
               Callback<mtx::responses::ContentURI> cb)
{
        std::map<std::string, std::string> params = {{"filename", filename}};

        const auto api_path = "/media/r0/upload?" + client::utils::query_params(params);
        post<std::string, mtx::responses::ContentURI>(api_path, data, cb, true, content_type);
}

void
Client::download(const std::string &mxc_url,
                 std::function<void(const std::string &res,
                                    const std::string &content_type,
                                    const std::string &original_filename,
                                    RequestErr err)> callback)
{
        auto url = mtx::client::utils::parse_mxc_url(mxc_url);
        download(url.server, url.media_id, std::move(callback));
}

void
Client::get_thumbnail(const ThumbOpts &opts, Callback<std::string> callback, bool try_download)
{
        std::map<std::string, std::string> params;
        params.emplace("width", std::to_string(opts.width));
        params.emplace("height", std::to_string(opts.height));
        params.emplace("method", opts.method);

        const auto mxc      = mtx::client::utils::parse_mxc_url(opts.mxc_url);
        const auto api_path = "/media/r0/thumbnail/" + mxc.server + "/" + mxc.media_id + "?" +
                              client::utils::query_params(params);
        get<std::string>(api_path,
                         [callback, try_download, mxc = std::move(mxc), _this = shared_from_this()](
                           const std::string &res, HeaderFields, RequestErr err) {
                                 if (err && try_download) {
                                         const int status_code = static_cast<int>(err->status_code);

                                         if (status_code == 404) {
                                                 _this->download(
                                                   mxc.server,
                                                   mxc.media_id,
                                                   [callback = std::move(callback)](
                                                     const std::string &res,
                                                     const std::string &, // content_type
                                                     const std::string &, // original_filename
                                                     RequestErr err) { callback(res, err); });
                                                 return;
                                         }
                                 }

                                 callback(res, err);
                         });
}

void
Client::download(const std::string &server,
                 const std::string &media_id,
                 std::function<void(const std::string &res,
                                    const std::string &content_type,
                                    const std::string &original_filename,
                                    RequestErr err)> callback)
{
        const auto api_path = "/media/r0/download/" + server + "/" + media_id;
        get<std::string>(
          api_path, [callback](const std::string &res, HeaderFields fields, RequestErr err) {
                  std::string content_type, original_filename;

                  if (fields) {
                          if (fields->find("Content-Type") != fields->end())
                                  content_type = fields->at("Content-Type").to_string();
                          if (fields->find("Content-Disposition") != fields->end()) {
                                  auto value = fields->at("Content-Disposition").to_string();

                                  std::vector<std::string> results;
                                  boost::split(results, value, [](char c) { return c == '='; });

                                  original_filename = results.back();
                          }
                  }

                  callback(res, content_type, original_filename, err);
          });
}

void
Client::start_typing(const std::string &room_id, uint64_t timeout, ErrCallback callback)
{
        using mtx::client::utils::url_encode;
        const auto api_path =
          "/client/r0/rooms/" + url_encode(room_id) + "/typing/" + url_encode(user_id_.to_string());

        mtx::requests::TypingNotification req;
        req.typing  = true;
        req.timeout = timeout;

        put<mtx::requests::TypingNotification>(api_path, req, callback);
}

void
Client::stop_typing(const std::string &room_id, ErrCallback callback)
{
        using mtx::client::utils::url_encode;
        const auto api_path =
          "/client/r0/rooms/" + url_encode(room_id) + "/typing/" + url_encode(user_id_.to_string());

        mtx::requests::TypingNotification req;
        req.typing = false;

        put<mtx::requests::TypingNotification>(api_path, req, callback);
}

void
Client::get_event(const std::string &room_id,
                  const std::string &event_id,
                  Callback<mtx::events::collections::TimelineEvents> callback)
{
        using namespace mtx::client::utils;
        const auto api_path =
          "/client/r0/rooms/" + url_encode(room_id) + "/event/" + url_encode(event_id);

        get<mtx::events::collections::TimelineEvent>(
          api_path,
          [callback](const mtx::events::collections::TimelineEvent &res,
                     HeaderFields,
                     RequestErr err) { callback(res.data, err); });
}

void
Client::messages(const MessagesOpts &opts, Callback<mtx::responses::Messages> callback)
{
        std::map<std::string, std::string> params;

        params.emplace("dir", to_string(opts.dir));

        if (!opts.from.empty())
                params.emplace("from", opts.from);
        if (!opts.to.empty())
                params.emplace("to", opts.to);
        if (opts.limit > 0)
                params.emplace("limit", std::to_string(opts.limit));
        if (!opts.filter.empty())
                params.emplace("filter", opts.filter);

        const auto api_path = "/client/r0/rooms/" + mtx::client::utils::url_encode(opts.room_id) +
                              "/messages?" + client::utils::query_params(params);

        get<mtx::responses::Messages>(
          api_path, [callback](const mtx::responses::Messages &res, HeaderFields, RequestErr err) {
                  callback(res, err);
          });
}

void
Client::upload_filter(const nlohmann::json &j, Callback<mtx::responses::FilterId> callback)
{
        const auto api_path =
          "/client/r0/user/" + mtx::client::utils::url_encode(user_id_.to_string()) + "/filter";

        post<nlohmann::json, mtx::responses::FilterId>(api_path, j, callback);
}

void
Client::read_event(const std::string &room_id, const std::string &event_id, ErrCallback callback)
{
        const auto api_path =
          "/client/r0/rooms/" + mtx::client::utils::url_encode(room_id) + "/read_markers";

        nlohmann::json body = {{"m.fully_read", event_id}, {"m.read", event_id}};

        post<nlohmann::json, mtx::responses::Empty>(
          api_path, body, [callback](const mtx::responses::Empty, RequestErr err) {
                  callback(err);
          });
}

void
Client::redact_event(const std::string &room_id,
                     const std::string &event_id,
                     Callback<mtx::responses::EventId> callback)
{
        const auto api_path = "/client/r0/rooms/" + mtx::client::utils::url_encode(room_id) +
                              "/redact/" + mtx::client::utils::url_encode(event_id) + "/" +
                              mtx::client::utils::url_encode(mtx::client::utils::random_token());

        json body = json::object();
        put<nlohmann::json, mtx::responses::EventId>(api_path, body, callback);
}

void
Client::registration(const std::string &user,
                     const std::string &pass,
                     Callback<mtx::responses::Register> callback)
{
        nlohmann::json req = {{"username", user}, {"password", pass}};

        post<nlohmann::json, mtx::responses::Register>("/client/r0/register", req, callback, false);
}

void
Client::registration(const std::string &user,
                     const std::string &pass,
                     const mtx::user_interactive::Auth &auth,
                     Callback<mtx::responses::Register> callback)
{
        nlohmann::json req = {{"username", user}, {"password", pass}, {"auth", auth}};

        post<nlohmann::json, mtx::responses::Register>("/client/r0/register", req, callback, false);
}

void
Client::send_to_device(const std::string &event_type,
                       const std::string &txn_id,
                       const nlohmann::json &body,
                       ErrCallback callback)
{
        const auto api_path = "/client/r0/sendToDevice/" +
                              mtx::client::utils::url_encode(event_type) + "/" +
                              mtx::client::utils::url_encode(txn_id);
        put<nlohmann::json>(api_path, body, callback);
}

//
// Group related endpoints.
//

void
Client::create_group(const std::string &localpart, Callback<mtx::responses::GroupId> cb)
{
        json req;
        req["localpart"] = localpart;

        post<nlohmann::json, mtx::responses::GroupId>("/client/r0/create_group", req, cb);
}

void
Client::joined_groups(Callback<mtx::responses::JoinedGroups> cb)
{
        get<mtx::responses::JoinedGroups>("/client/r0/joined_groups",
                                          [cb](const mtx::responses::JoinedGroups &res,
                                               HeaderFields,
                                               RequestErr err) { cb(res, err); });
}

void
Client::group_profile(const std::string &group_id, Callback<mtx::responses::GroupProfile> cb)
{
        get<mtx::responses::GroupProfile>("/client/r0/groups/" + group_id + "/profile",
                                          [cb](const mtx::responses::GroupProfile &res,
                                               HeaderFields,
                                               RequestErr err) { cb(res, err); });
}

void
Client::group_rooms(const std::string &group_id, Callback<nlohmann::json> cb)
{
        get<nlohmann::json>(
          "/client/r0/groups/" + group_id + "/rooms",
          [cb](const nlohmann::json &res, HeaderFields, RequestErr err) { cb(res, err); });
}

void
Client::set_group_profile(const std::string &group_id,
                          nlohmann::json &req,
                          Callback<nlohmann::json> cb)
{
        post<nlohmann::json, nlohmann::json>("/client/r0/groups/" + group_id + "/profile", req, cb);
}

void
Client::add_room_to_group(const std::string &room_id, const std::string &group_id, ErrCallback cb)
{
        put<nlohmann::json>(
          "/client/r0/groups/" + group_id + "/admin/rooms/" + room_id, json::object(), cb);
}

//
// Encryption related endpoints
//

void
Client::upload_keys(const mtx::requests::UploadKeys &req,
                    Callback<mtx::responses::UploadKeys> callback)
{
        post<mtx::requests::UploadKeys, mtx::responses::UploadKeys>(
          "/client/r0/keys/upload", req, callback);
}

void
Client::query_keys(const mtx::requests::QueryKeys &req,
                   Callback<mtx::responses::QueryKeys> callback)
{
        post<mtx::requests::QueryKeys, mtx::responses::QueryKeys>(
          "/client/r0/keys/query", req, callback);
}

//! Claims one-time keys for use in pre-key messages.
void
Client::claim_keys(const std::string &user,
                   const std::vector<std::string> &devices,
                   Callback<mtx::responses::ClaimKeys> cb)
{
        mtx::requests::ClaimKeys req;

        std::map<std::string, std::string> dev_to_algorithm;
        for (const auto &d : devices)
                dev_to_algorithm.emplace(d, "signed_curve25519");

        req.one_time_keys[user] = dev_to_algorithm;

        post<mtx::requests::ClaimKeys, mtx::responses::ClaimKeys>(
          "/client/r0/keys/claim", std::move(req), std::move(cb));
}

void
Client::key_changes(const std::string &from,
                    const std::string &to,
                    Callback<mtx::responses::KeyChanges> callback)
{
        std::map<std::string, std::string> params;

        if (!from.empty())
                params.emplace("from", from);

        if (!to.empty())
                params.emplace("to", to);

        get<mtx::responses::KeyChanges>(
          "/client/r0/keys/changes?" + mtx::client::utils::query_params(params),
          [callback](const mtx::responses::KeyChanges &res, HeaderFields, RequestErr err) {
                  callback(res, err);
          });
}

void
Client::enable_encryption(const std::string &room, Callback<mtx::responses::EventId> callback)
{
        using namespace mtx::events;
        state::Encryption event;

        send_state_event<state::Encryption, EventType::RoomEncryption>(room, "", event, callback);
}
