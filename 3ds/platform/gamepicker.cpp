// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: game picker on the bottom screen console (see gamepicker.h).
#include "gamepicker.h"

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <vector>

namespace {
const char *const SINGLE_GAME_DIR = "sdmc:/rmxp3ds/game";
const char *const GAMES_DIR = "sdmc:/rmxp3ds/games";
const char *const LAST_GAME_FILE = "sdmc:/rmxp3ds/last-game.txt";
const int LIST_ROWS = 22;  // bottom console: 30 rows; header and footer take the rest
const size_t LABEL_COLS = 36; // 40 columns minus the cursor and margins

struct Game {
	std::string dir;
	std::string label;
};

bool isDir(const std::string &path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isFile(const std::string &path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Title= from the [Game] section of dir/Game.ini, or "" if there is none. Bytes outside
// printable ASCII become '?' (the console font has ASCII only; titles may be CP1252 or UTF-8).
std::string iniTitle(const std::string &dir) {
	FILE *f = fopen((dir + "/Game.ini").c_str(), "rb");
	if (!f) return "";
	char line[512];
	bool inGame = false;
	std::string title;
	while (fgets(line, sizeof(line), f)) {
		std::string s(line);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
		if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
			s.erase(0, 3); // UTF-8 byte order mark
		if (!s.empty() && s[0] == '[') {
			inGame = strncasecmp(s.c_str(), "[Game]", 6) == 0;
			continue;
		}
		if (inGame && strncasecmp(s.c_str(), "Title=", 6) == 0) {
			title = s.substr(6);
			break;
		}
	}
	fclose(f);
	for (char &c : title)
		if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) c = '?';
	return title;
}

// A game directory: Game.ini or mkxp.json in it.
bool isGameDir(const std::string &dir) { return isFile(dir + "/Game.ini") || isFile(dir + "/mkxp.json"); }

std::vector<Game> findGames() {
	std::vector<Game> games;
	if (DIR *d = opendir(GAMES_DIR)) {
		while (struct dirent *e = readdir(d)) {
			if (e->d_name[0] == '.') continue;
			std::string dir = std::string(GAMES_DIR) + "/" + e->d_name;
			if (!isDir(dir) || !isGameDir(dir)) continue;
			std::string title = iniTitle(dir);
			games.push_back({dir, title.empty() ? std::string(e->d_name) : title});
		}
		closedir(d);
	}
	std::sort(games.begin(), games.end(),
	          [](const Game &a, const Game &b) { return strcasecmp(a.dir.c_str(), b.dir.c_str()) < 0; });
	// The single-game directory (kits, rmxp3ds runtime checks: they all write mkxp.json there).
	if (isGameDir(SINGLE_GAME_DIR)) {
		std::string title = iniTitle(SINGLE_GAME_DIR);
		games.push_back({SINGLE_GAME_DIR, (title.empty() ? std::string("game") : title) + " (rmxp3ds/game)"});
	}
	return games;
}

std::string readLastGame() {
	FILE *f = fopen(LAST_GAME_FILE, "rb");
	if (!f) return "";
	char line[512] = {0};
	if (!fgets(line, sizeof(line), f)) line[0] = 0;
	fclose(f);
	std::string s(line);
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
	return s;
}

void writeLastGame(const std::string &dir) {
	if (FILE *f = fopen(LAST_GAME_FILE, "wb")) {
		fprintf(f, "%s\n", dir.c_str());
		fclose(f);
	}
}

// Draws the list with the cursor on `sel`, scrolled so that it is visible.
void drawList(const std::vector<Game> &games, int sel) {
	consoleClear();
	printf("rmxp3ds: choose a game (%d)\n\n", (int)games.size());
	int first = std::max(0, std::min(sel - LIST_ROWS / 2, (int)games.size() - LIST_ROWS));
	int last = std::min((int)games.size(), first + LIST_ROWS);
	for (int i = first; i < last; ++i) {
		std::string label = games[i].label;
		if (label.size() > LABEL_COLS) label = label.substr(0, LABEL_COLS - 3) + "...";
		printf("%s %s\n", i == sel ? ">" : " ", label.c_str());
	}
	for (int i = last - first; i < LIST_ROWS; ++i) printf("\n");
	printf("\nUp/Down/Left/Right: select\nA: start   START: exit");
	gfxFlushBuffers(); // the console writes to the cached framebuffer (libctru console examples)
}
} // namespace

std::string rmxp3dsPickGame(std::string &error) {
	error.clear();
	std::vector<Game> games = findGames();
	if (games.empty()) {
		error = "no game found.\nCopy a converted game to\nsdmc:/rmxp3ds/games/<name>/";
		return "";
	}
	if (games.size() == 1) return games[0].dir;

	int sel = 0;
	std::string last = readLastGame();
	for (size_t i = 0; i < games.size(); ++i)
		if (games[i].dir == last) sel = (int)i;
	hidSetRepeatParameters(20, 5); // frames: first repeat after 1/3 s, then 12 per second
	drawList(games, sel);
	const int count = (int)games.size();
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		u32 repeat = hidKeysDownRepeat();
		int old = sel;
		if (repeat & KEY_DOWN) sel = (sel + 1) % count;
		if (repeat & KEY_UP) sel = (sel + count - 1) % count;
		if (repeat & KEY_RIGHT) sel = std::min(count - 1, sel + LIST_ROWS);
		if (repeat & KEY_LEFT) sel = std::max(0, sel - LIST_ROWS);
		if (sel != old) drawList(games, sel);
		if (down & KEY_A) {
			writeLastGame(games[sel].dir);
			consoleClear();
			return games[sel].dir;
		}
		if (down & KEY_START) break;
		gspWaitForVBlank();
	}
	consoleClear();
	return "";
}
