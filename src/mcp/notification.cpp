#include "notification.h"
#include <cmath>
namespace mcp::detail {
namespace {
void require(bool value) {
    if (!value)
        throw TransportError(TransportErrorKind::invalid_response, "Invalid request notification");
}
} // namespace
void validate_notification(const Json &value, const Json &request, NotificationState &state) {
    require(value.is_object() && value.value("jsonrpc", Json()) == "2.0" && !value.contains("id") &&
            value.contains("method") && value["method"].is_string() && value.contains("params") &&
            value["params"].is_object());
    const auto method = text(value["method"]);
    const auto &params = value["params"];
    if (request["method"] == "subscriptions/listen") {
        require(params.contains("_meta") && params["_meta"].is_object() &&
                params["_meta"].contains(subscription_key) &&
                params["_meta"][subscription_key] == request["id"]);
        if (method == "notifications/subscriptions/acknowledged") {
            require(!state.acknowledged && params.contains("notifications") &&
                    params["notifications"].is_object());
            const auto &requested = request["params"]["notifications"];
            for (auto it = params["notifications"].begin(); it != params["notifications"].end();
                 ++it) {
                require(requested.contains(it.key()));
                if (it.key() == "resourceSubscriptions" || it.key() == "taskIds") {
                    require(it.value().is_array() && requested[it.key()].is_array());
                    for (const auto &uri : it.value()) {
                        require(uri.is_string());
                        require(std::find(requested[it.key()].begin(), requested[it.key()].end(),
                                          uri) != requested[it.key()].end());
                    }
                } else {
                    require(it.key() == "toolsListChanged" || it.key() == "promptsListChanged" ||
                            it.key() == "resourcesListChanged");
                    require(it.value().is_boolean() && requested[it.key()].is_boolean() &&
                            (!it.value().get<bool>() || requested[it.key()].get<bool>()));
                }
            }
            state.honored = params["notifications"];
            state.acknowledged = true;
        } else {
            require(state.acknowledged);
            std::string_view flag;
            if (method == "notifications/tools/list_changed")
                flag = "toolsListChanged";
            else if (method == "notifications/prompts/list_changed")
                flag = "promptsListChanged";
            else if (method == "notifications/resources/list_changed")
                flag = "resourcesListChanged";
            else if (method == "notifications/resources/updated") {
                require(params.contains("uri") && params["uri"].is_string() &&
                        state.honored.contains("resourceSubscriptions"));
                const auto &uris = state.honored["resourceSubscriptions"];
                require(std::find(uris.begin(), uris.end(), params["uri"]) != uris.end());
            } else if (method == "notifications/tasks") {
                require(params.contains("taskId") && params["taskId"].is_string() &&
                        state.honored.contains("taskIds"));
                const auto &ids = state.honored["taskIds"];
                require(std::find(ids.begin(), ids.end(), params["taskId"]) != ids.end());
            } else
                require(false);
            if (!flag.empty())
                require(state.honored.value(flag, false));
        }
    } else if (method == "notifications/progress") {
        const auto &meta = request["params"]["_meta"];
        require(meta.contains("progressToken") &&
                params.value("progressToken", Json()) == meta["progressToken"] &&
                params.contains("progress") && params["progress"].is_number());
        const double progress = params["progress"].get<double>();
        require(std::isfinite(progress) && (!state.progress || progress > *state.progress));
        if (params.contains("total"))
            require(params["total"].is_number() && std::isfinite(params["total"].get<double>()));
        if (params.contains("message"))
            require(params["message"].is_string());
        state.progress = progress;
    } else if (method == "notifications/message") {
        require(request["params"]["_meta"].contains("io.modelcontextprotocol/logLevel") &&
                params.contains("level") && params["level"].is_string() && params.contains("data"));
    } else
        require(false);
}
void validate_stream_end(const Json &value, const Json &request, const NotificationState &state) {
    validate_response(value, request["id"]);
    if (request["method"] == "subscriptions/listen" && value.contains("result"))
        require(state.acknowledged &&
                value["result"].value("resultType", Json("complete")) == "complete");
}
} // namespace mcp::detail
