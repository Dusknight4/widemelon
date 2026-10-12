// WideMelon: keeps a game's save in two locations, with backups.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include <QString>

#include "types.h"

namespace WideMelon
{

// With save sync on, a game's save lives in two places: location 1, where
// WideMelon normally keeps it (next to the ROM, or the save folder from the
// path settings), and location 2, a folder of the user's choice such as the
// game folder on an R4 card's SD card. When a game loads, the newer of the
// two is used and copied over the older one; every save is written to both.
// Location 2 being unavailable (the card isn't inserted) is not an error: the
// save then only goes to location 1. Before a save file is replaced, the old
// one goes into Backups/Save Location 1 or 2 as "<name> Bak1.sav" (the
// newest) up to "<name> Bak5.sav".
struct SaveSyncSettings
{
    bool Enabled = false;
    QString SecondFolder; // location 2
    QString BackupFolder; // holds "Save Location 1" and "Save Location 2"
};

SaveSyncSettings LoadSaveSyncSettings();

// The save's path in location 2, or empty if there is none.
std::string SecondSavePath(const std::string& firstPath, const SaveSyncSettings& settings);

// Before loading the save at firstPath: makes both locations hold the newer save.
void SyncSavesForLoad(const std::string& firstPath, const SaveSyncSettings& settings);

// Writes a save, first moving the file it replaces into backupSubfolder of the
// backup folder (when backups are wanted). Returns false if it couldn't be written.
bool WriteSave(const std::string& path, const melonDS::u8* data, melonDS::u32 length,
               const QString& backupFolder);

}
