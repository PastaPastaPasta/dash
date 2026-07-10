// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/transport/grpcweb.h>

#include <platform/transport/tls.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace platform::transport {

namespace {

//! Wrap a protobuf message in a single gRPC-Web data frame.
std::vector<uint8_t> FrameMessage(const std::vector<uint8_t>& msg)
{
    std::vector<uint8_t> frame;
    frame.reserve(5 + msg.size());
    frame.push_back(0x00); // data frame, not compressed
    const uint32_t len = static_cast<uint32_t>(msg.size());
    frame.push_back(static_cast<uint8_t>(len >> 24));
    frame.push_back(static_cast<uint8_t>(len >> 16));
    frame.push_back(static_cast<uint8_t>(len >> 8));
    frame.push_back(static_cast<uint8_t>(len));
    frame.insert(frame.end(), msg.begin(), msg.end());
    return frame;
}

std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

GrpcCallResult GrpcWebUnary(const std::string& host, uint16_t port, const std::string& path,
                            const std::vector<uint8_t>& request, int timeout_ms)
{
    GrpcCallResult result;

    auto conn = TlsConnection::Connect(host, port, timeout_ms, result.transport_error);
    if (!conn) return result;

    const std::vector<uint8_t> body = FrameMessage(request);

    std::string head;
    head += "POST " + path + " HTTP/1.1\r\n";
    head += "Host: " + host + "\r\n";
    head += "Content-Type: application/grpc-web+proto\r\n";
    head += "Accept: application/grpc-web+proto\r\n";
    head += "X-Grpc-Web: 1\r\n";
    head += "Te: trailers\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";

    std::vector<uint8_t> out(head.begin(), head.end());
    out.insert(out.end(), body.begin(), body.end());
    if (!conn->WriteAll(out, result.transport_error)) return result;

    // Read the full response (Connection: close → read to EOF).
    std::vector<uint8_t> resp;
    std::vector<uint8_t> chunk(16384);
    for (;;) {
        int n = conn->Read(chunk, result.transport_error);
        if (n < 0) return result;
        if (n == 0) break;
        resp.insert(resp.end(), chunk.begin(), chunk.begin() + n);
        if (resp.size() > 32u * 1024 * 1024) { // guard
            result.transport_error = "response too large";
            return result;
        }
    }

    // Split HTTP headers from body.
    static const uint8_t sep[4] = {'\r', '\n', '\r', '\n'};
    auto it = std::search(resp.begin(), resp.end(), sep, sep + 4);
    if (it == resp.end()) {
        result.transport_error = "malformed HTTP response";
        return result;
    }
    const std::string headers(resp.begin(), it);
    std::vector<uint8_t> payload(it + 4, resp.end());

    // HTTP status line.
    {
        const auto eol = headers.find("\r\n");
        const std::string status_line = headers.substr(0, eol);
        // "HTTP/1.1 200 OK"
        const auto sp = status_line.find(' ');
        if (sp == std::string::npos || status_line.substr(sp + 1, 3) != "200") {
            result.transport_error = "HTTP error: " + status_line;
            // gRPC-Web may still deliver grpc-status in headers on non-200; but
            // treat as transport error for simplicity.
            return result;
        }
    }

    // Envoy may return the response chunked (Transfer-Encoding: chunked). Undo
    // chunk framing if present.
    if (ToLower(headers).find("transfer-encoding: chunked") != std::string::npos) {
        std::vector<uint8_t> dechunked;
        size_t p = 0;
        while (p < payload.size()) {
            // read hex length line
            size_t line_end = p;
            while (line_end + 1 < payload.size() && !(payload[line_end] == '\r' && payload[line_end + 1] == '\n')) {
                ++line_end;
            }
            if (line_end + 1 >= payload.size()) break;
            const std::string hexlen(payload.begin() + p, payload.begin() + line_end);
            size_t chunk_len = 0;
            try { chunk_len = std::stoul(hexlen, nullptr, 16); } catch (...) { break; }
            p = line_end + 2;
            if (chunk_len == 0) break;
            if (p + chunk_len > payload.size()) break;
            dechunked.insert(dechunked.end(), payload.begin() + p, payload.begin() + p + chunk_len);
            p += chunk_len + 2; // skip trailing CRLF
        }
        payload.swap(dechunked);
    }

    result.transport_ok = true;

    // Parse gRPC-Web frames: data frame(s) then a trailers frame (0x80).
    result.grpc_status = 0; // default OK unless a trailer overrides
    size_t p = 0;
    while (p + 5 <= payload.size()) {
        const uint8_t flags = payload[p];
        const uint32_t len = (static_cast<uint32_t>(payload[p + 1]) << 24) |
                             (static_cast<uint32_t>(payload[p + 2]) << 16) |
                             (static_cast<uint32_t>(payload[p + 3]) << 8) |
                             static_cast<uint32_t>(payload[p + 4]);
        p += 5;
        if (p + len > payload.size()) break;
        Span<const uint8_t> frame(payload.data() + p, len);
        p += len;
        if (flags & 0x80) {
            // Trailers frame: "grpc-status: N\r\ngrpc-message: ...\r\n"
            const std::string trailers(frame.begin(), frame.end());
            const auto lower = ToLower(trailers);
            auto spos = lower.find("grpc-status:");
            if (spos != std::string::npos) {
                result.grpc_status = std::atoi(trailers.c_str() + spos + 12);
            }
            auto mpos = lower.find("grpc-message:");
            if (mpos != std::string::npos) {
                auto start = mpos + 13;
                while (start < trailers.size() && trailers[start] == ' ') ++start;
                auto end = trailers.find("\r\n", start);
                result.grpc_message = trailers.substr(start, end == std::string::npos ? std::string::npos : end - start);
            }
        } else {
            result.message.assign(frame.begin(), frame.end());
        }
    }

    return result;
}

} // namespace platform::transport
