// MultiMelon: Azahar's multiplayer rooms for 3DS games in the menu (see RoomDialogs.h).
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "RoomDialogs.h"
#include "ThreeDSRoom.h"
#include "Config.h"
#include "main.h"

// ---- the rooms joined before (history) and favourites: rooms.json beside MultiMelon's settings ----

namespace
{
struct RoomEntry
{
    QString address;
    int port = ThreeDSRoom::DefaultPort;
    QString nickname;       // the one used there
    QString password;       // kept for favourites only
    QString room;           // its name, as last seen
    QStringList people;     // who else was in it, last time
    QDateTime last;
    int times = 0;
    bool favorite = false;

    QString key() const { return address.trimmed().toLower() + ":" + QString::number(port); }
};

constexpr int kMaxRecent = 30;

QString StorePath() { return emuDirectory + "/rooms.json"; }

QList<RoomEntry> LoadRooms()
{
    QList<RoomEntry> list;
    QFile f(StorePath());
    if (!f.open(QIODevice::ReadOnly)) return list;
    const QJsonArray rooms = QJsonDocument::fromJson(f.readAll()).object().value("rooms").toArray();
    for (const QJsonValue& v : rooms)
    {
        const QJsonObject o = v.toObject();
        RoomEntry e;
        e.address = o.value("address").toString();
        e.port = o.value("port").toInt(ThreeDSRoom::DefaultPort);
        e.nickname = o.value("nickname").toString();
        e.password = o.value("password").toString();
        e.room = o.value("room").toString();
        for (const QJsonValue& p : o.value("people").toArray()) e.people << p.toString();
        e.last = QDateTime::fromString(o.value("last").toString(), Qt::ISODate);
        e.times = o.value("times").toInt();
        e.favorite = o.value("favorite").toBool();
        if (!e.address.isEmpty()) list << e;
    }
    return list;
}

void SaveRooms(QList<RoomEntry> list)
{
    std::stable_sort(list.begin(), list.end(), [](const RoomEntry& a, const RoomEntry& b) { return a.last > b.last; });
    QJsonArray rooms;
    int recent = 0;
    for (const RoomEntry& e : list)
    {
        if (!e.favorite && ++recent > kMaxRecent) continue;     // (the oldest non-favourites go)
        QJsonObject o;
        o["address"] = e.address;
        o["port"] = e.port;
        o["nickname"] = e.nickname;
        if (e.favorite && !e.password.isEmpty()) o["password"] = e.password;
        o["room"] = e.room;
        o["people"] = QJsonArray::fromStringList(e.people);
        o["last"] = e.last.toString(Qt::ISODate);
        o["times"] = e.times;
        o["favorite"] = e.favorite;
        rooms << o;
    }
    QSaveFile f(StorePath());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(QJsonObject{{"rooms", rooms}}).toJson());
    f.commit();
}

RoomEntry& FindOrAdd(QList<RoomEntry>& list, const QString& address, int port)
{
    RoomEntry probe;
    probe.address = address;
    probe.port = port;
    for (RoomEntry& e : list)
        if (e.key() == probe.key()) return e;
    list << probe;
    return list.last();
}

// what the history keeps once a join has worked
void RecordJoin(const QString& address, int port, const QString& nickname, const ThreeDSRoom::Info& info)
{
    QList<RoomEntry> list = LoadRooms();
    RoomEntry& e = FindOrAdd(list, address.trimmed(), port);
    e.nickname = nickname;
    e.room = QString::fromStdString(info.roomName);
    e.people.clear();
    for (const auto& m : info.members)
        if (QString::fromStdString(m.nickname) != nickname) e.people << QString::fromStdString(m.nickname);
    e.last = QDateTime::currentDateTime();
    e.times++;
    SaveRooms(list);
}

QString DefaultNickname()
{
    QString n = QString::fromStdString(Config::GetGlobalTable().GetString("MP.Room.Nickname"));
    if (!n.isEmpty()) return n;
    // (the Windows user name, with only what rooms accept)
    QString out;
    for (const QChar c : qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER")))
        if ((c.unicode() < 128 && c.isLetterOrNumber()) || QStringLiteral(" ._-").contains(c)) out += c;
    return out.left(20);
}

// this PC's addresses on its networks, for friends on the same network
QStringList LocalAddresses()
{
    QStringList out;
    for (const QHostAddress& a : QNetworkInterface::allAddresses())
    {
        if (a.protocol() != QAbstractSocket::IPv4Protocol || a.isLoopback()) continue;
        const QString s = a.toString();
        if (!s.startsWith("169.254.")) out << s;
    }
    return out;
}

bool CheckNames(QWidget* parent, const QString& title, const QString& nickname, const QString* roomName = nullptr)
{
    const QString rule = "4 to 20 letters, digits, spaces, dots, dashes or underscores";
    if (roomName && !ThreeDSRoom::ValidName(roomName->toStdString()))
    {
        QMessageBox::information(parent, title, "The room name needs " + rule + ".");
        return false;
    }
    if (!ThreeDSRoom::ValidName(nickname.toStdString()))
    {
        QMessageBox::information(parent, title, "Your nickname needs " + rule + ".");
        return false;
    }
    return true;
}

// in a room already: leave it first? (false: stay)
bool LeaveCurrent(QWidget* parent, const QString& title)
{
    const ThreeDSRoom::Info i = ThreeDSRoom::GetInfo();
    if (!ThreeDSRoom::InRoom() && !i.hosting) return true;
    const QString what = i.hosting ? "You host a room now. Close it" : "You're in a room now. Leave it";
    if (QMessageBox::question(parent, title, what + " first?") != QMessageBox::Yes) return false;
    ThreeDSRoom::Leave();
    return true;
}
} // namespace

// ---- events to the GUI thread ----

RoomUi::Notifier* RoomUi::Notifier::get()
{
    static Notifier* n = [] {
        auto* created = new Notifier();
        ThreeDSRoom::SetEventHandler([created](int kind, const std::string& text) {
            // (emitted on the room's network thread; receivers in the GUI thread get it queued)
            emit created->roomEvent(kind, QString::fromStdString(text));
        });
        return created;
    }();
    return n;
}

// ---- Create Room ----

CreateRoomDialog* CreateRoomDialog::current = nullptr;

void CreateRoomDialog::openDlg(QWidget* parent)
{
    if (current)
    {
        current->show();
        current->raise();
        current->activateWindow();
        return;
    }
    current = new CreateRoomDialog(parent);
    current->show();
}

CreateRoomDialog::CreateRoomDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Create Room (3DS games)");
    setAttribute(Qt::WA_DeleteOnClose);
    connect(this, &QObject::destroyed, [] { current = nullptr; });
    RoomUi::Notifier::get();

    auto cfg = Config::GetGlobalTable();
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel("Open a room on this PC for a 3DS game's local wireless over the internet (as in "
                             "Azahar). Friends join it with Direct Connect, in MultiMelon or Azahar. Everyone then "
                             "picks the game's local wireless (Host or Join) as if they were in the same room.");
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    const QString savedName = QString::fromStdString(cfg.GetString("MP.Room.Name"));
    name = new QLineEdit(savedName.isEmpty() ? "MultiMelon room" : savedName);
    nickname = new QLineEdit(DefaultNickname());
    nickname->setPlaceholderText("what the others see");
    port = new QSpinBox();
    port->setRange(1, 65535);
    port->setValue(cfg.GetInt("MP.Room.Port") > 0 ? cfg.GetInt("MP.Room.Port") : ThreeDSRoom::DefaultPort);
    players = new QSpinBox();
    players->setRange(2, 16);
    players->setValue(cfg.GetInt("MP.Room.Slots") >= 2 ? cfg.GetInt("MP.Room.Slots") : 4);
    password = new QLineEdit();
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText("none");
    form->addRow("Room name:", name);
    form->addRow("Your nickname:", nickname);
    form->addRow("Port (UDP):", port);
    form->addRow("Players:", players);
    form->addRow("Password (optional):", password);
    layout->addLayout(form);

    const QStringList here = LocalAddresses();
    auto* how = new QLabel();
    how->setWordWrap(true);
    how->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto updateHow = [this, how, here] {
        const QString p = QString::number(port->value());
        how->setText(QString("<b>How friends join:</b> Direct Connect to this PC, port %1.<br>"
                             "&bull; On your network: %2<br>"
                             "&bull; Elsewhere: your internet address (search \"what is my IP\"). Your router must "
                             "forward UDP port %1 to this PC.<br>"
                             "If Windows asks whether MultiMelon may use the network, allow it.")
                         .arg(p, here.isEmpty() ? QString("(this PC's address)") : here.join(", ")));
    };
    connect(port, &QSpinBox::valueChanged, this, updateHow);
    updateHow();
    layout->addWidget(how);

    status = new QLabel();
    status->setWordWrap(true);
    status->setStyleSheet("color: #b03030");
    layout->addWidget(status);

    auto* buttons = new QDialogButtonBox();
    auto* createBtn = buttons->addButton("Create Room", QDialogButtonBox::AcceptRole);
    auto* cancelBtn = buttons->addButton(QDialogButtonBox::Cancel);
    connect(createBtn, &QPushButton::clicked, this, &CreateRoomDialog::onCreate);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::close);
    layout->addWidget(buttons);
    resize(520, sizeHint().height());
}

void CreateRoomDialog::onCreate()
{
    const QString roomName = name->text().trimmed(), nick = nickname->text().trimmed();
    if (!CheckNames(this, windowTitle(), nick, &roomName)) return;
    if (!LeaveCurrent(this, windowTitle())) return;

    const std::string err = ThreeDSRoom::Host(roomName.toStdString(), nick.toStdString(), (uint16_t)port->value(),
                                              players->value(), password->text().toStdString());
    if (!err.empty())
    {
        status->setText(QString::fromStdString(err));
        return;
    }
    auto cfg = Config::GetGlobalTable();
    cfg.SetString("MP.Room.Name", roomName.toStdString());
    cfg.SetString("MP.Room.Nickname", nick.toStdString());
    cfg.SetInt("MP.Room.Port", port->value());
    cfg.SetInt("MP.Room.Slots", players->value());
    Config::Save();
    RoomWindow::openDlg(parentWidget());
    close();
}

// ---- Direct Connect ----

DirectConnectDialog* DirectConnectDialog::current = nullptr;

void DirectConnectDialog::openDlg(QWidget* parent)
{
    if (current)
    {
        current->show();
        current->raise();
        current->activateWindow();
        return;
    }
    current = new DirectConnectDialog(parent);
    current->show();
}

static QTreeWidget* MakeRoomList()
{
    auto* t = new QTreeWidget();
    t->setColumnCount(4);
    t->setHeaderLabels({"Room", "Address", "Who was there", "Last joined"});
    t->setRootIsDecorated(false);
    t->setUniformRowHeights(true);
    t->setAllColumnsShowFocus(true);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->header()->setStretchLastSection(false);
    t->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    t->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    t->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    t->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    t->setMinimumHeight(90);
    return t;
}

DirectConnectDialog::DirectConnectDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Direct Connect to Room (3DS games)");
    setAttribute(Qt::WA_DeleteOnClose);
    connect(this, &QObject::destroyed, [] { current = nullptr; });
    connect(RoomUi::Notifier::get(), &RoomUi::Notifier::roomEvent, this, &DirectConnectDialog::onRoomEvent);

    auto cfg = Config::GetGlobalTable();
    auto* layout = new QVBoxLayout(this);

    auto* form = new QFormLayout();
    address = new QLineEdit(QString::fromStdString(cfg.GetString("MP.Room.JoinAddress")));
    address->setPlaceholderText("the host's address, e.g. 203.0.113.7 or 192.168.1.20");
    port = new QSpinBox();
    port->setRange(1, 65535);
    port->setValue(cfg.GetInt("MP.Room.JoinPort") > 0 ? cfg.GetInt("MP.Room.JoinPort") : ThreeDSRoom::DefaultPort);
    nickname = new QLineEdit(DefaultNickname());
    nickname->setPlaceholderText("what the others see");
    password = new QLineEdit();
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText("if the room has one");
    form->addRow("Address:", address);
    form->addRow("Port (UDP):", port);
    form->addRow("Your nickname:", nickname);
    form->addRow("Password:", password);
    layout->addLayout(form);

    layout->addWidget(new QLabel("<b>Favorites</b>"));
    favorites = MakeRoomList();
    layout->addWidget(favorites);
    layout->addWidget(new QLabel("<b>Recent</b> (rooms you joined)"));
    recent = MakeRoomList();
    layout->addWidget(recent);

    auto* listButtons = new QHBoxLayout();
    favoriteBtn = new QPushButton("Add to Favorites");
    removeBtn = new QPushButton("Forget");
    listButtons->addWidget(favoriteBtn);
    listButtons->addWidget(removeBtn);
    listButtons->addStretch();
    layout->addLayout(listButtons);

    status = new QLabel();
    status->setWordWrap(true);
    layout->addWidget(status);

    auto* buttons = new QDialogButtonBox();
    connectBtn = buttons->addButton("Connect", QDialogButtonBox::AcceptRole);
    auto* closeBtn = buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    connect(connectBtn, &QPushButton::clicked, this, &DirectConnectDialog::onConnect);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(favoriteBtn, &QPushButton::clicked, this, &DirectConnectDialog::onFavorite);
    connect(removeBtn, &QPushButton::clicked, this, &DirectConnectDialog::onRemove);
    for (QTreeWidget* list : {favorites, recent})
    {
        connect(list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) { onPick(item); });
        connect(list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
            onPick(item);
            onConnect();
        });
    }
    reload();
    resize(640, 560);
}

QTreeWidgetItem* DirectConnectDialog::selected() const
{
    if (!favorites->selectedItems().isEmpty()) return favorites->selectedItems().first();
    if (!recent->selectedItems().isEmpty()) return recent->selectedItems().first();
    return nullptr;
}

void DirectConnectDialog::reload()
{
    QList<RoomEntry> list = LoadRooms();
    std::stable_sort(list.begin(), list.end(), [](const RoomEntry& a, const RoomEntry& b) { return a.last > b.last; });
    favorites->clear();
    recent->clear();
    for (const RoomEntry& e : list)
    {
        const QString where = e.address + ":" + QString::number(e.port);
        const QStringList cols = {e.room.isEmpty() ? QString("(unknown)") : e.room, where,
                                  e.people.isEmpty() ? QString("-") : e.people.join(", "),
                                  e.last.isValid() ? e.last.toString("MMM d, h:mm AP") : QString("-")};
        auto add = [&](QTreeWidget* to) {
            auto* item = new QTreeWidgetItem(to, cols);
            item->setData(0, Qt::UserRole, e.key());
            item->setToolTip(0, QString("Joined %1 time(s) as %2").arg(e.times).arg(e.nickname));
        };
        if (e.favorite) add(favorites);
        add(recent);
    }
    favoriteBtn->setText("Add to Favorites");
    removeBtn->setEnabled(false);
}

void DirectConnectDialog::onPick(QTreeWidgetItem* item)
{
    if (!item) return;
    (item->treeWidget() == favorites ? recent : favorites)->clearSelection();
    const QString key = item->data(0, Qt::UserRole).toString();
    for (const RoomEntry& e : LoadRooms())
    {
        if (e.key() != key) continue;
        address->setText(e.address);
        port->setValue(e.port);
        if (!e.nickname.isEmpty()) nickname->setText(e.nickname);
        password->setText(e.favorite ? e.password : QString());
        favoriteBtn->setText(e.favorite ? "Remove from Favorites" : "Add to Favorites");
        removeBtn->setEnabled(true);
        break;
    }
}

void DirectConnectDialog::onFavorite()
{
    const QString addr = address->text().trimmed();
    if (addr.isEmpty())
    {
        status->setText("Type an address (or pick a room) to add it to Favorites.");
        return;
    }
    QList<RoomEntry> list = LoadRooms();
    RoomEntry& e = FindOrAdd(list, addr, port->value());
    e.favorite = !e.favorite;
    if (e.favorite)
    {
        // (the password and nickname typed now are kept with it)
        e.password = password->text();
        if (!nickname->text().trimmed().isEmpty()) e.nickname = nickname->text().trimmed();
        if (!e.last.isValid()) e.last = QDateTime::currentDateTime();
    }
    else
        e.password.clear();
    SaveRooms(list);
    reload();
}

void DirectConnectDialog::onRemove()
{
    QTreeWidgetItem* item = selected();
    if (!item) return;
    const QString key = item->data(0, Qt::UserRole).toString();
    QList<RoomEntry> list = LoadRooms();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const RoomEntry& e) { return e.key() == key; }), list.end());
    SaveRooms(list);
    reload();
}

void DirectConnectDialog::onConnect()
{
    const QString addr = address->text().trimmed(), nick = nickname->text().trimmed();
    if (addr.isEmpty())
    {
        QMessageBox::information(this, windowTitle(), "Type the address of the room's host (or pick a room).");
        return;
    }
    if (!CheckNames(this, windowTitle(), nick)) return;
    if (!LeaveCurrent(this, windowTitle())) return;

    auto cfg = Config::GetGlobalTable();
    cfg.SetString("MP.Room.JoinAddress", addr.toStdString());
    cfg.SetInt("MP.Room.JoinPort", port->value());
    cfg.SetString("MP.Room.Nickname", nick.toStdString());
    Config::Save();

    waiting = true;
    connectBtn->setEnabled(false);
    status->setStyleSheet("");
    status->setText(QString("Connecting to %1:%2...").arg(addr).arg(port->value()));
    ThreeDSRoom::Join(addr.toStdString(), (uint16_t)port->value(), nick.toStdString(), password->text().toStdString());
}

void DirectConnectDialog::onRoomEvent(int kind, QString text)
{
    if (!waiting || kind != 0) return;
    const ThreeDSRoom::Info info = ThreeDSRoom::GetInfo();
    if (info.state == ThreeDSRoom::State::Joined)
    {
        waiting = false;
        RecordJoin(address->text().trimmed(), port->value(), nickname->text().trimmed(), info);
        RoomWindow::openDlg(parentWidget());
        close();
    }
    else if (info.state == ThreeDSRoom::State::Failed || (info.state == ThreeDSRoom::State::Idle && !text.isEmpty()))
    {
        waiting = false;
        connectBtn->setEnabled(true);
        status->setStyleSheet("color: #b03030");
        status->setText(text.isEmpty() ? QString::fromStdString(info.error) : text);
    }
}

// ---- the current room ----

RoomWindow* RoomWindow::current = nullptr;

void RoomWindow::openDlg(QWidget* parent)
{
    if (current)
    {
        current->show();
        current->raise();
        current->activateWindow();
        return;
    }
    current = new RoomWindow(parent);
    current->show();
}

RoomWindow::RoomWindow(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Room (3DS games)");
    setAttribute(Qt::WA_DeleteOnClose);
    connect(this, &QObject::destroyed, [] { current = nullptr; });
    connect(RoomUi::Notifier::get(), &RoomUi::Notifier::roomEvent, this, &RoomWindow::onRoomEvent);

    auto* layout = new QVBoxLayout(this);
    heading = new QLabel();
    heading->setWordWrap(true);
    heading->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(heading);

    auto* row = new QHBoxLayout();
    auto* left = new QVBoxLayout();
    left->addWidget(new QLabel("<b>In the room</b>"));
    members = new QListWidget();
    members->setMaximumWidth(200);
    left->addWidget(members);
    row->addLayout(left);

    auto* right = new QVBoxLayout();
    right->addWidget(new QLabel("<b>Chat</b>"));
    chat = new QTextEdit();
    chat->setReadOnly(true);
    right->addWidget(chat);
    auto* sendRow = new QHBoxLayout();
    message = new QLineEdit();
    message->setPlaceholderText("Say something to the room");
    message->setMaxLength(500);
    sendBtn = new QPushButton("Send");
    sendRow->addWidget(message);
    sendRow->addWidget(sendBtn);
    right->addLayout(sendRow);
    row->addLayout(right, 1);
    layout->addLayout(row);

    auto* hint = new QLabel("Join the room before entering the game's local wireless: a session already open on "
                            "\"Wifi as Local Multiplayer\" doesn't move into the room.");
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray");
    layout->addWidget(hint);

    auto* buttons = new QDialogButtonBox();
    leaveBtn = buttons->addButton("Leave Room", QDialogButtonBox::DestructiveRole);
    auto* closeBtn = buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    connect(sendBtn, &QPushButton::clicked, this, &RoomWindow::onSend);
    connect(message, &QLineEdit::returnPressed, this, &RoomWindow::onSend);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(leaveBtn, &QPushButton::clicked, this, [this] {
        ThreeDSRoom::Leave();
        refresh();
        close();
    });
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &RoomWindow::refresh);
    timer->start(1000);
    refresh();
    resize(600, 420);
}

void RoomWindow::refresh()
{
    const ThreeDSRoom::Info i = ThreeDSRoom::GetInfo();
    const QString where = QString::fromStdString(i.address) + ":" + QString::number(i.port);
    switch (i.state)
    {
    case ThreeDSRoom::State::Joined:
        heading->setText(QString("<b>%1</b>: %2 of %3 players<br>%4")
                             .arg(QString::fromStdString(i.roomName).toHtmlEscaped())
                             .arg(i.members.size())
                             .arg(i.capacity)
                             .arg(i.hosting ? QString("You host this room (UDP port %1).").arg(i.port)
                                            : QString("Joined at %1.").arg(where)));
        break;
    case ThreeDSRoom::State::Joining: heading->setText(QString("Joining %1...").arg(where)); break;
    default:
        heading->setText(i.error.empty() ? QString("Not in a room.")
                                         : QString("Not in a room: %1").arg(QString::fromStdString(i.error)));
        break;
    }
    members->clear();
    for (const auto& m : i.members)
    {
        QString line = QString::fromStdString(m.nickname);
        if (!m.game.empty()) line += " (" + QString::fromStdString(m.game) + ")";
        members->addItem(line);
    }
    const bool joined = i.state == ThreeDSRoom::State::Joined;
    message->setEnabled(joined);
    sendBtn->setEnabled(joined);
    leaveBtn->setEnabled(ThreeDSRoom::InRoom() || i.hosting);
}

void RoomWindow::onRoomEvent(int kind, QString text)
{
    if (kind == 1)
        chat->append(text.toHtmlEscaped());
    else if (kind == 2)
        chat->append("<i>" + text.toHtmlEscaped() + "</i>");
    else if (!text.isEmpty())
        chat->append("<span style=\"color:#b03030\">" + text.toHtmlEscaped() + "</span>");
    refresh();
}

void RoomWindow::onSend()
{
    const QString text = message->text().trimmed();
    if (text.isEmpty()) return;
    ThreeDSRoom::SendChat(text.toStdString());
    // (the room sends it to everyone else: ours goes in here)
    const QString me = QString::fromStdString(Config::GetGlobalTable().GetString("MP.Room.Nickname"));
    chat->append("<b>" + me.toHtmlEscaped() + ":</b> " + text.toHtmlEscaped());
    message->clear();
}

// ---- (testing) the dialogs as pictures ----

void RoomUi::Snapshot(const QString& dir)
{
    auto grab = [&](QDialog* d, const QString& name) {
        d->setAttribute(Qt::WA_DontShowOnScreen);   // (laid out and drawn, never on screen: no window, no focus)
        d->show();
        QCoreApplication::processEvents();
        d->grab().save(dir + "/" + name + ".png");
        d->close();
        QCoreApplication::processEvents();
    };
    grab(new CreateRoomDialog(nullptr), "create_room");
    auto* dc = new DirectConnectDialog(nullptr);
    if (dc->favorites->topLevelItemCount()) dc->onPick(dc->favorites->topLevelItem(0));
    if (dc->favorites->topLevelItemCount()) dc->favorites->topLevelItem(0)->setSelected(true);
    grab(dc, "direct_connect");
    auto* rw = new RoomWindow(nullptr);
    rw->chat->append("<i>Alex joined the room</i>");
    rw->chat->append("Alex: ready when you are");
    rw->chat->append("<b>Sam:</b> starting the dungeon");
    grab(rw, "room_window");
}
