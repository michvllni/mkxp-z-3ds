// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: Audio for the Nintendo 3DS (replaces src/audio/*.cpp, which need OpenAL).
// Silent for now: the calls succeed and do nothing, positions are 0 and volumes are remembered.
// ndsp-based audio (BGM streamed from SD, SE cached) follows in a later milestone (PLAN §3.1).
#include "audio.h"

struct AudioPrivate {
	int bgmVolume = 100;
};

Audio::Audio(RGSSThreadData &) : p(new AudioPrivate) {}
Audio::~Audio() { delete p; }

void Audio::bgmPlay(const char *, int volume, int, double, int) { p->bgmVolume = volume; }
void Audio::bgmStop(int) {}
void Audio::bgmFade(int, int) {}
int Audio::bgmGetVolume(int) { return p->bgmVolume; }
void Audio::bgmSetVolume(int volume, int) { p->bgmVolume = volume; }
void Audio::bgsPlay(const char *, int, int, double) {}
void Audio::bgsStop() {}
void Audio::bgsFade(int) {}
void Audio::mePlay(const char *, int, int) {}
void Audio::meStop() {}
void Audio::meFade(int) {}
void Audio::sePlay(const char *, int, int) {}
void Audio::seStop() {}
void Audio::setupMidi() {}
double Audio::bgmPos(int) { return 0; }
double Audio::bgsPos() { return 0; }
void Audio::reset() {}
