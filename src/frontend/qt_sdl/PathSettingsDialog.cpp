/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QTemporaryFile>

#include "types.h"
#include "Config.h"
#include "Platform.h"
#include "main.h"

#include "PathSettingsDialog.h"
#include "ui_PathSettingsDialog.h"

using namespace melonDS::Platform;
namespace Platform = melonDS::Platform;

PathSettingsDialog* PathSettingsDialog::currentDlg = nullptr;

bool PathSettingsDialog::needsReset = false;

constexpr char errordialog[] = "melonDS cannot write to that directory.";

PathSettingsDialog::PathSettingsDialog(QWidget* parent) : QDialog(parent), ui(new Ui::PathSettingsDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();

    auto& cfg = emuInstance->getLocalConfig();
    ui->txtSaveFilePath->setText(cfg.GetQString("SaveFilePath"));
    ui->txtSavestatePath->setText(cfg.GetQString("SavestatePath"));
    ui->txtCheatFilePath->setText(cfg.GetQString("CheatFilePath"));

    int inst = emuInstance->getInstanceID();
    if (inst > 0)
        ui->lblInstanceNum->setText(QString("Configuring paths for instance %1").arg(inst+1));
    else
        ui->lblInstanceNum->hide();

    // WideMelon: a second save location kept in sync with the first
    auto savesBox = new QGroupBox("Second save location");
    auto savesLayout = new QGridLayout(savesBox);
    saveSync = new QCheckBox("Keep saves in two locations in sync");
    saveSync->setToolTip("Location 1 is the save files path above (or the ROM's folder). When a game loads, the newer "
                         "save of the two is used and copied over the older one, and every save is written to both, "
                         "so you can move between WideMelon and an R4 card's SD card. If location 2 isn't there, saves "
                         "just go to location 1. Replaced saves are kept in the Backups folder (Bak1 is the newest, up to Bak5).");
    secondSaveFolder = new QLineEdit;
    secondSaveFolder->setPlaceholderText("For example the game folder on your SD card");
    secondSaveFolder->setClearButtonEnabled(true);
    auto browse = new QPushButton("Browse...");
    savesLayout->addWidget(saveSync, 0, 0, 1, 3);
    savesLayout->addWidget(new QLabel("Save location 2:"), 1, 0);
    savesLayout->addWidget(secondSaveFolder, 1, 1);
    savesLayout->addWidget(browse, 1, 2);
    ui->gridLayout->addWidget(savesBox, 5, 0, 1, 3);
    connect(browse, &QPushButton::clicked, this, [this]
    {
        QString dir = QFileDialog::getExistingDirectory(this, "Select save location 2...", secondSaveFolder->text());
        if (!dir.isEmpty()) secondSaveFolder->setText(QDir::toNativeSeparators(dir));
    });

    auto global = Config::GetGlobalTable();
    saveSync->setChecked(global.GetBool("WideMelon.SaveSync"));
    secondSaveFolder->setText(global.GetQString("WideMelon.SecondSaveFolder"));
    saveSync->setProperty("user_originalValue", saveSync->isChecked());

#define SET_ORIGVAL(type, val) \
    for (type* w : findChildren<type*>(nullptr)) \
        w->setProperty("user_originalValue", w->val());

    SET_ORIGVAL(QLineEdit, text);

#undef SET_ORIGVAL
}

PathSettingsDialog::~PathSettingsDialog()
{
    delete ui;
}

void PathSettingsDialog::done(int r)
{
    if (!((MainWindow*)parent())->getEmuInstance())
    {
        QDialog::done(r);
        closeDlg();
        return;
    }
    
    needsReset = false;

    if (r == QDialog::Accepted)
    {
        bool modified = false;

#define CHECK_ORIGVAL(type, val) \
        if (!modified) for (type* w : findChildren<type*>(nullptr)) \
        {                        \
            QVariant v = w->val();                   \
            if (v != w->property("user_originalValue")) \
            {                    \
                modified = true; \
                break;                   \
            }\
        }

        CHECK_ORIGVAL(QLineEdit, text);
        CHECK_ORIGVAL(QCheckBox, isChecked);

#undef CHECK_ORIGVAL

        if (modified)
        {
            if (emuInstance->emuIsActive()
                && QMessageBox::warning(this, "Reset necessary to apply changes",
                    "The emulation will be reset for the changes to take place.",
                    QMessageBox::Ok, QMessageBox::Cancel) != QMessageBox::Ok)
                return;

            auto& cfg = emuInstance->getLocalConfig();
            cfg.SetQString("SaveFilePath", ui->txtSaveFilePath->text());
            cfg.SetQString("SavestatePath", ui->txtSavestatePath->text());
            cfg.SetQString("CheatFilePath", ui->txtCheatFilePath->text());

            auto global = Config::GetGlobalTable();
            global.SetBool("WideMelon.SaveSync", saveSync->isChecked());
            global.SetQString("WideMelon.SecondSaveFolder", secondSaveFolder->text().trimmed());

            Config::Save();

            needsReset = true;
        }
    }

    QDialog::done(r);

    closeDlg();
}

void PathSettingsDialog::on_btnSaveFileBrowse_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(this,
                                                     "Select save files path...",
                                                     emuDirectory);

    if (dir.isEmpty()) return;
    
    if (!QTemporaryFile(dir).open())
    {
        QMessageBox::critical(this, "melonDS", errordialog);
        return;
    }

    ui->txtSaveFilePath->setText(dir);
}

void PathSettingsDialog::on_btnSavestateBrowse_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(this,
                                                     "Select savestates path...",
                                                     emuDirectory);

    if (dir.isEmpty()) return;
    
    if (!QTemporaryFile(dir).open())
    {
        QMessageBox::critical(this, "melonDS", errordialog);
        return;
    }

    ui->txtSavestatePath->setText(dir);
}

void PathSettingsDialog::on_btnCheatFileBrowse_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(this,
                                                     "Select cheat files path...",
                                                     emuDirectory);

    if (dir.isEmpty()) return;
    
    if (!QTemporaryFile(dir).open())
    {
        QMessageBox::critical(this, "melonDS", errordialog);
        return;
    }

    ui->txtCheatFilePath->setText(dir);
}
