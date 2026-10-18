#include <cstdint>
#include <mcp/sse.h>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    try {
        mcp::SseDecoder decoder([](std::string_view) {}, 4096);
        const std::string_view bytes(reinterpret_cast<const char *>(data), size);
        for (std::size_t i = 0; i < bytes.size();) {
            const auto count = std::min<std::size_t>(1 + static_cast<unsigned char>(bytes[i]) % 31,
                                                     bytes.size() - i);
            decoder.feed(bytes.substr(i, count));
            i += count;
        }
        decoder.finish();
    } catch (const mcp::TransportError &) {
    }
    return 0;
}
