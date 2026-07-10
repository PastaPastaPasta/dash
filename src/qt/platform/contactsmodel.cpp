// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactsmodel.h>

#include <qt/platform/platformservice.h>

ContactsModel::ContactsModel(PlatformService& service, QObject* parent) :
    QAbstractTableModel(parent),
    m_service(service)
{
    connect(&m_service, &PlatformService::contactsUpdated, this,
            [this](const QVector<QPair<QString, QString>>& incoming,
                   const QVector<QPair<QString, QString>>& outgoing) {
                if (!incoming.isEmpty()) m_incoming = incoming;
                if (!outgoing.isEmpty()) m_outgoing = outgoing;
                rebuild();
            });
}

int ContactsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int ContactsModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : COLUMN_COUNT;
}

QVariant ContactsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    if (role != Qt::DisplayRole) return {};
    const Row& r = m_rows[index.row()];
    switch (index.column()) {
    case Username: return r.username.isEmpty() ? r.identity_hex.left(16) + "…" : r.username;
    case DisplayName: return r.display_name;
    case Direction:
        switch (r.kind) {
        case Kind::Established: return tr("Contact");
        case Kind::Incoming: return tr("Request received");
        case Kind::Outgoing: return tr("Request sent");
        }
        return {};
    }
    return {};
}

QVariant ContactsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case Username: return tr("Username");
    case DisplayName: return tr("Display name");
    case Direction: return tr("Status");
    }
    return {};
}

void ContactsModel::refresh()
{
    m_service.refreshContacts();
}

void ContactsModel::onIncoming(const QVector<QPair<QString, QString>>& contacts) { m_incoming = contacts; rebuild(); }
void ContactsModel::onOutgoing(const QVector<QPair<QString, QString>>& contacts) { m_outgoing = contacts; rebuild(); }

void ContactsModel::rebuild()
{
    beginResetModel();
    m_rows.clear();
    // Requests appearing both incoming and outgoing are established contacts.
    for (const auto& in : m_incoming) {
        bool established = false;
        for (const auto& out : m_outgoing) {
            if (out.first == in.first) { established = true; break; }
        }
        m_rows.append({in.first, in.second, {}, established ? Kind::Established : Kind::Incoming});
    }
    for (const auto& out : m_outgoing) {
        bool seen = false;
        for (const auto& r : m_rows) {
            if (r.identity_hex == out.first) { seen = true; break; }
        }
        if (!seen) m_rows.append({out.first, out.second, {}, Kind::Outgoing});
    }
    endResetModel();
}
