#include <mcp/sse.h>

namespace mcp {
struct SseDecoder::Impl {
    std::function<void(std::string_view)> callback;
    std::size_t limit;
    std::string line, data, event;
    bool after_cr = false;
    bool first_line = true;
    bool has_data = false;
    Impl(std::function<void(std::string_view)> cb, std::size_t bytes)
        : callback(std::move(cb)), limit(bytes) {}
    void consume() {
        if (first_line) {
            first_line = false;
            if (line.starts_with("\xEF\xBB\xBF"))
                line.erase(0, 3);
        }
        if (line.empty()) {
            if (has_data && (event.empty() || event == "message")) {
                if (!data.empty())
                    data.pop_back();
                callback(data);
            }
            data.clear();
            event.clear();
            has_data = false;
        } else if (line.front() != ':') {
            const auto colon = line.find(':');
            std::string_view field(line.data(), colon == std::string::npos ? line.size() : colon);
            std::string_view value = colon == std::string::npos
                                         ? std::string_view()
                                         : std::string_view(line).substr(colon + 1);
            if (value.starts_with(' '))
                value.remove_prefix(1);
            if (field == "data") {
                if (value.size() + 1 > limit - data.size())
                    throw TransportError(TransportErrorKind::limit, "SSE event exceeds byte limit");
                data.append(value);
                data += '\n';
                has_data = true;
            } else if (field == "event")
                event = value;
        }
        line.clear();
    }
};
SseDecoder::SseDecoder(std::function<void(std::string_view)> cb, std::size_t bytes)
    : impl_(std::make_unique<Impl>(std::move(cb), bytes)) {
    if (!impl_->callback || !bytes)
        throw std::invalid_argument("SSE callback and limit are required");
}
SseDecoder::~SseDecoder() = default;
void SseDecoder::feed(std::string_view bytes) {
    auto &state = *impl_;
    for (char c : bytes) {
        if (state.after_cr) {
            state.after_cr = false;
            if (c == '\n')
                continue;
        }
        if (c == '\r' || c == '\n') {
            state.consume();
            state.after_cr = c == '\r';
        } else {
            if (state.line.size() >= state.limit)
                throw TransportError(TransportErrorKind::limit, "SSE line exceeds byte limit");
            state.line += c;
        }
    }
}
void SseDecoder::finish() {
    // SSE dispatch requires an empty line; an incomplete final event is discarded.
    impl_->line.clear();
    impl_->data.clear();
    impl_->event.clear();
    impl_->has_data = false;
}
} // namespace mcp
