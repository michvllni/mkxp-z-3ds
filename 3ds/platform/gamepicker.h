// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: chooses the game directory when the app is started without one (rmxp3ds M5).
#pragma once

#include <string>

// Looks for games in sdmc:/rmxp3ds/games/<name>/ (packages written by rmxp3ds-convert: a
// directory with Game.ini or mkxp.json) and in the single-game directory sdmc:/rmxp3ds/game
// (kits and tests). One game: returns it. Several: shows a list on the bottom screen console
// (D-pad, A starts, START leaves) and returns the chosen one; the choice is remembered in
// sdmc:/rmxp3ds/last-game.txt and preselected next time. Returns "" if there is no game (error
// is set) or the user left (error stays empty).
std::string rmxp3dsPickGame(std::string &error);
