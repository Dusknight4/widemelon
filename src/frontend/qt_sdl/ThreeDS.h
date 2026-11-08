/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a fork of melonDS.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#pragma once

#include <string>

// MultiMelon: 3DS games, through Azahar's emulation core (github.com/azahar-emu/azahar).
// Only ThreeDS.cpp sees Azahar's headers; the rest of the frontend uses this.
//
// The OpenGL context must be current on the calling thread for Load(), RunFrame() and
// Unload(): Azahar draws both screens into a framebuffer of ours (top screen above the
// bottom one, Azahar's "Default" layout), and CopyScreens() puts them into a texture the
// screen panel shows the way it shows a DS's: a 2-layer array texture, layer 0 the top
// screen (400x240 scaled), layer 1 the touchscreen (320x240 scaled) in the middle of
// its layer, which the panel's shader samples as it does a DS touchscreen.
namespace ThreeDS
{

// files the 3DS core opens (by extension): .3ds .cci .cxi .3dsx .app .elf .axf, and .cia, which is
// installed into the virtual 3DS first (as on a 3DS), then its game started
bool IsThreeDSFile(const std::string& path);

// once, before anything else: where 3DS saves, the virtual SD card and NAND go
void Init(const std::string& userDir);

// "" when the game is running, else what went wrong (for the user)
std::string Load(const std::string& path);
void Unload();
bool Running();
// starts the running game again from the beginning
std::string Reset();

// savestates (Azahar's, of the running game): false if it didn't work
bool SaveState(const std::string& path);
bool LoadState(const std::string& path);

// what the player does this frame. Buttons: bits 0-11 as the DS's key mask (A, B, Select,
// Start, Right, Left, Up, Down, R, L, X, Y), set = pressed; bit 12 ZL, 13 ZR, 14 Home.
// Circle Pad and C-Stick: -1..1, up positive. Touch: on the touchscreen, 0..319 x 0..239.
struct Input
{
    unsigned buttons = 0;
    float circleX = 0.f, circleY = 0.f;
    float cstickX = 0.f, cstickY = 0.f;
    bool touching = false;
    float touchX = 0.f, touchY = 0.f;
};

// runs the game until it shows its next frame
void RunFrame(const Input& input);

// the texture described above (made or resized as needed), after the last frame
unsigned CopyScreens();
// its size: the top screen's (400x240 times the internal resolution)
int TextureWidth();
int TextureHeight();

// at the end (after Unload): stops what Init started (Azahar's log thread, which mustn't outlive main)
void Finish();

// internal resolution (1 = the 3DS's own, 2 = twice it...); takes effect at the next Load()
void SetResolution(int factor);

// speed in percent of the 3DS's own (100 = normal, 0 = as fast as it goes), and the sound: volume
// 0..1, muted or not (from the frontend's fast forward, slow motion, frame limiter, mute and volume)
void SetSpeed(double percent);
void SetAudio(float volume, bool muted);

}
