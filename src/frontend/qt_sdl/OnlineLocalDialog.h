/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#ifndef ONLINELOCALDIALOG_H
#define ONLINELOCALDIALOG_H

#include <QDialog>

class QRadioButton;
class QLineEdit;
class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;
class EmuThread;

// Multiplayer > Online as Local: host or join a room (name + password) on a room server,
// to play a game's local wireless with a friend over the internet (net/OnlineLocal.h).
// Stays connected when closed; opening it again shows how it's going.
class OnlineLocalDialog : public QDialog
{
    Q_OBJECT

public:
    static void openDlg(QWidget* parent, EmuThread* emu);

private:
    OnlineLocalDialog(QWidget* parent, EmuThread* emu);
    void onConnect();
    void refresh();

    static OnlineLocalDialog* current;
    EmuThread* emu;
    QRadioButton* hostBtn;
    QRadioButton* joinBtn;
    QLineEdit* room;
    QLineEdit* password;
    QLineEdit* server;
    QCheckBox* runServer;
    QLabel* status;
    QPushButton* connectBtn;
    QTimer* timer;
};

#endif
