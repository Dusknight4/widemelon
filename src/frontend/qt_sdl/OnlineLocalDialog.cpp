/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>

#include "OnlineLocalDialog.h"
#include "OnlineLocal.h"
#include "EmuThread.h"
#include "Config.h"

OnlineLocalDialog* OnlineLocalDialog::current = nullptr;

void OnlineLocalDialog::openDlg(QWidget* parent, EmuThread* emu)
{
    if (current)
    {
        current->show();
        current->raise();
        current->activateWindow();
        return;
    }
    current = new OnlineLocalDialog(parent, emu);
    current->show();
}

OnlineLocalDialog::OnlineLocalDialog(QWidget* parent, EmuThread* emu) : QDialog(parent), emu(emu)
{
    setWindowTitle("Online as Local");
    setAttribute(Qt::WA_DeleteOnClose);
    connect(this, &QObject::destroyed, [] { current = nullptr; });

    auto cfg = Config::GetGlobalTable();
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel("Play a game's local wireless (Union Room trades, battles...) with a friend's "
                             "MultiMelon over the internet. You both type the same room name and password: "
                             "one of you hosts, the other joins. Connect before entering the Union Room.");
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* roleRow = new QHBoxLayout();
    hostBtn = new QRadioButton("Host (open the room)");
    joinBtn = new QRadioButton("Join (my friend hosts)");
    (cfg.GetBool("MP.Online.Join") ? joinBtn : hostBtn)->setChecked(true);
    roleRow->addWidget(hostBtn);
    roleRow->addWidget(joinBtn);
    layout->addLayout(roleRow);

    auto* form = new QFormLayout();
    room = new QLineEdit(QString::fromStdString(cfg.GetString("MP.Online.Room")));
    password = new QLineEdit();
    password->setEchoMode(QLineEdit::Password);
    server = new QLineEdit(QString::fromStdString(cfg.GetString("MP.Online.Server")));
    server->setPlaceholderText("address of the room server, e.g. 203.0.113.7 or rooms.example.net:7300");
    form->addRow("Room name:", room);
    form->addRow("Room password:", password);
    form->addRow("Room server:", server);
    layout->addLayout(form);

    runServer = new QCheckBox("Run the room server on this PC (UDP port 7300 must be forwarded to it for a friend elsewhere)");
    runServer->setChecked(cfg.GetBool("MP.Online.RunServer"));
    layout->addWidget(runServer);

    status = new QLabel();
    status->setWordWrap(true);
    status->setMinimumHeight(40);
    layout->addWidget(status);

    auto* buttons = new QDialogButtonBox();
    connectBtn = buttons->addButton("Connect", QDialogButtonBox::AcceptRole);
    auto* closeBtn = buttons->addButton("Close", QDialogButtonBox::RejectRole);
    connect(connectBtn, &QPushButton::clicked, this, &OnlineLocalDialog::onConnect);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    layout->addWidget(buttons);

    auto syncRole = [this] { runServer->setEnabled(hostBtn->isChecked() && !OnlineLocal::Enabled()); };
    connect(hostBtn, &QRadioButton::toggled, this, syncRole);
    syncRole();

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &OnlineLocalDialog::refresh);
    timer->start(300);
    refresh();
    resize(520, sizeHint().height());
}

void OnlineLocalDialog::onConnect()
{
    const bool running = emu->emuIsRunning();
    if (OnlineLocal::Enabled())
    {
        if (running) emu->emuPause();
        OnlineLocal::Stop();
        if (running) emu->emuUnpause();
        refresh();
        return;
    }
    const std::string name = room->text().trimmed().toStdString(), pass = password->text().toStdString();
    if (name.empty() || pass.empty())
    {
        QMessageBox::information(this, "Online as Local", "Type a room name and a password (the same ones your friend types).");
        return;
    }
    std::string srv = server->text().trimmed().toStdString();
    const bool host = hostBtn->isChecked(), runHere = host && runServer->isChecked();
    if (srv.empty() && !runHere)
    {
        QMessageBox::information(this, "Online as Local", "Type the room server's address (or, as host, run the room server on this PC).");
        return;
    }
    if (srv.empty()) srv = "127.0.0.1";

    auto cfg = Config::GetGlobalTable();
    cfg.SetBool("MP.Online.Join", !host);
    cfg.SetString("MP.Online.Room", name);
    cfg.SetString("MP.Online.Server", server->text().trimmed().toStdString());
    cfg.SetBool("MP.Online.RunServer", runServer->isChecked());
    Config::Save();

    OnlineLocal::Settings s;
    s.Server = srv;
    s.Host = host;
    s.RunServer = runHere;
    OnlineLocal::RoomIdFrom(name, pass, s.RoomId);
    if (running) emu->emuPause();
    OnlineLocal::Start(s);
    if (running) emu->emuUnpause();
    refresh();
}

void OnlineLocalDialog::refresh()
{
    const bool on = OnlineLocal::Enabled();
    status->setText(on ? QString::fromStdString(OnlineLocal::StatusText()) : "Not connected.");
    connectBtn->setText(on ? "Disconnect" : "Connect");
    for (QWidget* w : {(QWidget*)hostBtn, (QWidget*)joinBtn, (QWidget*)room, (QWidget*)password, (QWidget*)server})
        w->setEnabled(!on);
    runServer->setEnabled(!on && hostBtn->isChecked());
}
