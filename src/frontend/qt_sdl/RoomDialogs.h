// MultiMelon: Azahar's multiplayer rooms for 3DS games in the menu: Create Room, Direct Connect (with the rooms
// joined before, and favourites), and the current room (who's in it, chat).
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QObject>
#include <QString>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace RoomUi
{
// the room's events (ThreeDSRoom.h), brought to the GUI thread: kind 0 state, 1 chat line, 2 joined/left
class Notifier : public QObject
{
    Q_OBJECT
public:
    static Notifier* get();
signals:
    void roomEvent(int kind, QString text);
};

// (testing) each dialog drawn into dir/<name>.png, never shown on screen (MULTIMELON_UI_SNAPSHOT)
void Snapshot(const QString& dir);
}

class CreateRoomDialog : public QDialog
{
    Q_OBJECT
public:
    static void openDlg(QWidget* parent);

private:
    friend void RoomUi::Snapshot(const QString& dir);
    explicit CreateRoomDialog(QWidget* parent);
    void onCreate();

    static CreateRoomDialog* current;
    QLineEdit* name;
    QLineEdit* nickname;
    QSpinBox* port;
    QSpinBox* players;
    QLineEdit* password;
    QLabel* status;
};

class DirectConnectDialog : public QDialog
{
    Q_OBJECT
public:
    static void openDlg(QWidget* parent);

private:
    friend void RoomUi::Snapshot(const QString& dir);
    explicit DirectConnectDialog(QWidget* parent);
    void reload();
    void onPick(QTreeWidgetItem* item);
    void onConnect();
    void onFavorite();
    void onRemove();
    void onRoomEvent(int kind, QString text);
    QTreeWidgetItem* selected() const;

    static DirectConnectDialog* current;
    QLineEdit* address;
    QSpinBox* port;
    QLineEdit* nickname;
    QLineEdit* password;
    QTreeWidget* favorites;
    QTreeWidget* recent;
    QPushButton* connectBtn;
    QPushButton* favoriteBtn;
    QPushButton* removeBtn;
    QLabel* status;
    bool waiting = false;
};

class RoomWindow : public QDialog
{
    Q_OBJECT
public:
    static void openDlg(QWidget* parent);

private:
    friend void RoomUi::Snapshot(const QString& dir);
    explicit RoomWindow(QWidget* parent);
    void refresh();
    void onRoomEvent(int kind, QString text);
    void onSend();

    static RoomWindow* current;
    QLabel* heading;
    QListWidget* members;
    QTextEdit* chat;
    QLineEdit* message;
    QPushButton* sendBtn;
    QPushButton* leaveBtn;
};
