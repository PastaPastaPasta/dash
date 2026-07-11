// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/transport/tls.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <util/strencodings.h>
#include <util/string.h>

#include <cstring>

namespace platform::transport {

struct TlsConnection::Impl {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    bool net_open{false};

    Impl()
    {
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_entropy_init(&entropy);
    }
    ~Impl()
    {
        if (net_open) mbedtls_ssl_close_notify(&ssl);
        mbedtls_net_free(&net);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }
};

TlsConnection::TlsConnection() : m_impl(std::make_unique<Impl>()) {}
TlsConnection::~TlsConnection() = default;

std::unique_ptr<TlsConnection> TlsConnection::Connect(const std::string& host, uint16_t port,
                                                      int timeout_ms, std::string& error)
{
    auto conn = std::unique_ptr<TlsConnection>(new TlsConnection());
    Impl& s = *conn->m_impl;

    const char* pers = "dash-platform-gui";
    if (mbedtls_ctr_drbg_seed(&s.ctr_drbg, mbedtls_entropy_func, &s.entropy,
                              reinterpret_cast<const unsigned char*>(pers), std::strlen(pers)) != 0) {
        error = "ctr_drbg seed failed";
        return nullptr;
    }

    const std::string port_str = ToString(port);
    if (mbedtls_net_connect(&s.net, host.c_str(), port_str.c_str(), MBEDTLS_NET_PROTO_TCP) != 0) {
        error = "tcp connect to " + host + ":" + port_str + " failed";
        return nullptr;
    }
    s.net_open = true;
    // Blocking socket; per-read timeouts are enforced through
    // mbedtls_net_recv_timeout (configured via ssl_conf_read_timeout).
    mbedtls_net_set_block(&s.net);

    if (mbedtls_ssl_config_defaults(&s.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        error = "ssl config defaults failed";
        return nullptr;
    }
    // See the class comment: transport certs are not chain-verified; response
    // integrity is guaranteed by proof + quorum-signature verification.
    mbedtls_ssl_conf_authmode(&s.conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s.conf, mbedtls_ctr_drbg_random, &s.ctr_drbg);
    mbedtls_ssl_conf_read_timeout(&s.conf, timeout_ms > 0 ? static_cast<uint32_t>(timeout_ms) : 0);

    if (mbedtls_ssl_setup(&s.ssl, &s.conf) != 0) {
        error = "ssl setup failed";
        return nullptr;
    }
    mbedtls_ssl_set_hostname(&s.ssl, host.c_str());
    mbedtls_ssl_set_bio(&s.ssl, &s.net, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&s.ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char buf[128];
            mbedtls_strerror(ret, buf, sizeof(buf));
            error = std::string("tls handshake failed: ") + buf;
            return nullptr;
        }
    }
    return conn;
}

bool TlsConnection::WriteAll(Span<const uint8_t> data, std::string& error)
{
    size_t written = 0;
    while (written < data.size()) {
        int ret = mbedtls_ssl_write(&m_impl->ssl, data.data() + written, data.size() - written);
        if (ret > 0) {
            written += static_cast<size_t>(ret);
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        char buf[128];
        mbedtls_strerror(ret, buf, sizeof(buf));
        error = std::string("tls write failed: ") + buf;
        return false;
    }
    return true;
}

int TlsConnection::Read(Span<uint8_t> buf, std::string& error)
{
    for (;;) {
        int ret = mbedtls_ssl_read(&m_impl->ssl, buf.data(), buf.size());
        if (ret >= 0) return ret;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        char b[128];
        mbedtls_strerror(ret, b, sizeof(b));
        error = std::string("tls read failed: ") + b;
        return -1;
    }
}

} // namespace platform::transport
