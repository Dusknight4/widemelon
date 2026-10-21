// WideMelon: keeps a game's save in two locations, with backups.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SaveSync.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstring>

#include "Config.h"
#include "Platform.h"

extern QString emuDirectory;

namespace WideMelon
{

namespace
{

constexpr int kBackups = 5;

QString backupName(const QFileInfo& file, int n)
{
    QString name = file.completeBaseName() + QString(" Bak%1").arg(n);
    if (!file.suffix().isEmpty())
        name += "." + file.suffix();
    return name;
}

// Moves the file at path into the backups as Bak1, shifting older ones up to Bak5.
void backUp(const QString& path, const QString& backupFolder)
{
    QDir dir(backupFolder);
    if (!dir.mkpath("."))
        return;
    const QFileInfo file(path);
    dir.remove(backupName(file, kBackups));
    for (int n = kBackups - 1; n >= 1; n--)
        dir.rename(backupName(file, n), backupName(file, n + 1));
    QFile::copy(path, dir.filePath(backupName(file, 1)));
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

// Copies one save over the other, backing up the one it replaces.
void copySave(const QString& from, const QString& to, const QString& backupFolder)
{
    const QByteArray data = readAll(from);
    if (data.isEmpty())
        return;
    WriteSave(to.toStdString(), reinterpret_cast<const melonDS::u8*>(data.constData()),
              static_cast<melonDS::u32>(data.size()), backupFolder);
}

}

SaveSyncSettings LoadSaveSyncSettings()
{
    auto global = Config::GetGlobalTable();
    SaveSyncSettings settings;
    settings.Enabled = global.GetBool("WideMelon.SaveSync");
    settings.SecondFolder = global.GetQString("WideMelon.SecondSaveFolder");
    settings.BackupFolder = QDir(emuDirectory).filePath("Backups");
    return settings;
}

std::string SecondSavePath(const std::string& firstPath, const SaveSyncSettings& settings)
{
    if (!settings.Enabled || settings.SecondFolder.isEmpty() || firstPath.empty())
        return {};
    const QString name = QFileInfo(QString::fromStdString(firstPath)).fileName();
    const QString second = QDir(settings.SecondFolder).filePath(name);
    // the same file in both locations needs no syncing
    if (QFileInfo(second).absoluteFilePath() == QFileInfo(QString::fromStdString(firstPath)).absoluteFilePath())
        return {};
    return second.toStdString();
}

void SyncSavesForLoad(const std::string& firstPath, const SaveSyncSettings& settings)
{
    const std::string secondPath = SecondSavePath(firstPath, settings);
    if (secondPath.empty() || !QDir(settings.SecondFolder).exists())
        return;

    const QString first = QString::fromStdString(firstPath), second = QString::fromStdString(secondPath);
    const QFileInfo firstInfo(first), secondInfo(second);
    const QString backups1 = QDir(settings.BackupFolder).filePath("Save Location 1");
    const QString backups2 = QDir(settings.BackupFolder).filePath("Save Location 2");

    if (!secondInfo.exists())
        return; // written there with the next save
    if (!firstInfo.exists())
    {
        copySave(second, first, backups1);
        return;
    }
    if (readAll(first) == readAll(second))
        return;
    if (secondInfo.lastModified() > firstInfo.lastModified())
        copySave(second, first, backups1);
    else
        copySave(first, second, backups2);
}

bool WriteSave(const std::string& path, const melonDS::u8* data, melonDS::u32 length,
               const QString& backupFolder)
{
    const QString file = QString::fromStdString(path);
    if (!backupFolder.isEmpty() && QFileInfo::exists(file))
    {
        const QByteArray old = readAll(file);
        if (old.size() != static_cast<qsizetype>(length) || memcmp(old.constData(), data, length) != 0)
            backUp(file, backupFolder);
    }

    QFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    const bool ok = f.write(reinterpret_cast<const char*>(data), length) == static_cast<qint64>(length);
    f.close();
    return ok;
}

}
