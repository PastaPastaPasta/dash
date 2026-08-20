// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/protxsender.h>

#include <interfaces/node.h>
#include <qt/walletmodel.h>
#include <rpc/protocol.h>
#include <util/threadnames.h>

#include <QTimer>
#include <QUrl>

namespace {
//! Consensus/RPC reject substrings worth a friendlier explanation than the raw error
struct KnownError {
    const char* needle;
    const char* explanation;
};
const KnownError KNOWN_ERRORS[]{
    {"bad-protx-dup-key", QT_TRANSLATE_NOOP("ProTxSender", "One of the chosen keys is already in use by a registered masternode or share. Every owner and voting key may be used only once network-wide.")},
    {"bad-protx-dup-addr", QT_TRANSLATE_NOOP("ProTxSender", "The chosen service address is already in use by a registered masternode.")},
    {"bad-protx-shares-payee-reuse", QT_TRANSLATE_NOOP("ProTxSender", "A refund or reward address may not double as a share owner or voting address. Use distinct addresses for payouts and keys.")},
    {"bad-protx-shares-sig", QT_TRANSLATE_NOOP("ProTxSender", "A participant's consent signature does not match the final transaction. This happens when the terms or the funding transaction changed after signing; collect fresh signatures.")},
    {"bad-protx-version", QT_TRANSLATE_NOOP("ProTxSender", "The network does not accept this transaction version yet. Wait for the v24 hard fork to activate.")},
    {"too-early", QT_TRANSLATE_NOOP("ProTxSender", "This feature is not active on the network yet. Wait for the v24 hard fork to activate.")},
    {"bad-prodis-dup", QT_TRANSLATE_NOOP("ProTxSender", "A dissolution for this masternode is already pending.")},
};
} // anonymous namespace

//! Runs on ProTxSender's worker thread; the only place executeRpc is called
class ProTxWorker : public QObject
{
    Q_OBJECT

public:
    explicit ProTxWorker(interfaces::Node& node) : m_node{node} {}

public Q_SLOTS:
    void execute(const QString& method, const QString& params_json, const QString& uri)
    {
        ProTxResult ret;
        try {
            UniValue params(UniValue::VOBJ);
            if (!params_json.isEmpty() && !params.read(params_json.toStdString())) {
                throw std::runtime_error("internal error: invalid parameter encoding");
            }
            ret.value = m_node.executeRpc(method.toStdString(), params, uri.toStdString());
            ret.ok = true;
        } catch (UniValue& err) {
            // JSONRPCError shape: {code, message}
            if (const auto& code{err.find_value("code")}; code.isNum()) {
                ret.code = code.getInt<int>();
            }
            QString message;
            if (const auto& msg{err.find_value("message")}; msg.isStr()) {
                message = QString::fromStdString(msg.get_str());
            } else {
                message = QString::fromStdString(err.write());
            }
            ret.message = ProTxSender::translateError(ret.code, message);
        } catch (const std::exception& e) {
            ret.code = RPC_MISC_ERROR;
            ret.message = QString::fromStdString(e.what());
        }
        Q_EMIT result(ret);
    }

Q_SIGNALS:
    void result(const ProTxResult& result);

private:
    interfaces::Node& m_node;
};

ProTxSender::ProTxSender(interfaces::Node& node, QObject* parent) :
    QObject(parent)
{
    qRegisterMetaType<ProTxResult>("ProTxResult");

    auto* worker = new ProTxWorker(node);
    worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(this, &ProTxSender::executeRequested, worker, &ProTxWorker::execute);
    connect(worker, &ProTxWorker::result, this, [this](const ProTxResult& result) {
        m_busy = false;
        Q_EMIT finished(result);
    });

    m_thread.start();
    QTimer::singleShot(0, worker, [] { util::ThreadRename("qt-protxsender"); });
}

ProTxSender::~ProTxSender()
{
    m_thread.quit();
    m_thread.wait();
}

bool ProTxSender::execute(const QString& method, const UniValue& params, const WalletModel* wallet_model)
{
    if (m_busy) return false;
    m_busy = true;

    QString uri;
    if (wallet_model) {
        const QByteArray encoded_name{QUrl::toPercentEncoding(wallet_model->getWalletName())};
        uri = "/wallet/" + QString::fromUtf8(encoded_name);
    }
    Q_EMIT executeRequested(method, QString::fromStdString(params.write()), uri);
    return true;
}

QString ProTxSender::translateError(int code, const QString& message)
{
    for (const auto& known : KNOWN_ERRORS) {
        if (message.contains(QLatin1String(known.needle))) {
            return tr(known.explanation) + "\n\n" + tr("Details: %1").arg(message);
        }
    }
    switch (code) {
    case RPC_IN_WARMUP:
        return tr("The node is still starting up. Try again once it has finished.");
    case RPC_METHOD_NOT_FOUND:
        return tr("This command is not available. Wallet support may be disabled.");
    case RPC_WALLET_UNLOCK_NEEDED:
        return tr("The wallet needs to be unlocked for this action.");
    default:
        return message;
    }
}

#include <qt/protxsender.moc>
