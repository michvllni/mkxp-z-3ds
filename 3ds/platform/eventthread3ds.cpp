// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: EventThread for the Nintendo 3DS (replaces src/eventthread.cpp in the 3DS build).
// There are no SDL windows or SDL events on the 3DS. The main thread runs process(): it reads
// the buttons and the circle pad with libctru's hid service once per frame and publishes them
// as an always-connected game controller (EventThread::controllerState), which mkxp-z's Input
// maps through its default controller bindings (src/input/keybindings.cpp). It also handles the
// APT loop: when the system asks the app to close, it requests termination of the RGSS thread.
#include "eventthread.h"

#include <3ds.h>
#include <SDL_atomic.h>
#include <SDL_mutex.h>
#include <stdio.h>
#include <string.h>

#include "debugwriter.h"
#include "platform3ds.h"
#include "sharedstate.h"

uint8_t EventThread::keyStates[];
EventThread::ControllerState EventThread::controllerState;
EventThread::MouseState EventThread::mouseState;
EventThread::TouchState EventThread::touchState;
SDL_atomic_t EventThread::verticalScrollDistance;

namespace {
// 3DS button -> SDL game controller button (default bindings: A = Input::C (confirm),
// B = Input::B (cancel), X = Input::A, Y = Input::X, L/R = Input::L/R).
struct ButtonMap {
	u32 key;
	SDL_GameControllerButton button;
};
const ButtonMap buttonMap[] = {
	{KEY_A, SDL_CONTROLLER_BUTTON_A},
	{KEY_B, SDL_CONTROLLER_BUTTON_B},
	{KEY_X, SDL_CONTROLLER_BUTTON_X},
	{KEY_Y, SDL_CONTROLLER_BUTTON_Y},
	{KEY_L, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
	{KEY_R, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
	{KEY_ZL, SDL_CONTROLLER_BUTTON_LEFTSTICK},
	{KEY_ZR, SDL_CONTROLLER_BUTTON_RIGHTSTICK},
	{KEY_START, SDL_CONTROLLER_BUTTON_START},
	{KEY_SELECT, SDL_CONTROLLER_BUTTON_BACK},
	// libctru's KEY_UP etc. are D-pad | circle pad: both move (RGSS has no analog movement).
	{KEY_UP, SDL_CONTROLLER_BUTTON_DPAD_UP},
	{KEY_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN},
	{KEY_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT},
	{KEY_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
};
// Circle pad range is about +-155; SDL axes are +-32767.
int scaleAxis(int v) {
	int s = v * 32767 / 155;
	return s > 32767 ? 32767 : (s < -32768 ? -32768 : s);
}
} // namespace

bool EventThread::allocUserEvents() { return true; }

EventThread::EventThread()
    : fullscreen(true), showCursor(false), ctrl(nullptr), textInputLock(SDL_CreateMutex()) {}

EventThread::~EventThread() { SDL_DestroyMutex(textInputLock); }

// Main-thread loop: input once per vertical blank until the RGSS thread is asked to stop
// (APT exit request) or has finished (rqTermAck).
void EventThread::process(RGSSThreadData &rtData) {
	platform3ds::installAptHook(rtData);
	memset(&controllerState, 0, sizeof controllerState);
	memset(keyStates, 0, sizeof keyStates);
	while (!rtData.rqTermAck) {
		if (!aptMainLoop()) {
			rtData.rqTerm.set();
			break;
		}
		hidScanInput();
		u32 held = hidKeysHeld();
		ControllerState next;
		memset(&next, 0, sizeof next);
		for (const ButtonMap &m : buttonMap)
			if (held & m.key) next.buttons[m.button] = true;
		circlePosition cp;
		hidCircleRead(&cp);
		next.axes[SDL_CONTROLLER_AXIS_LEFTX] = scaleAxis(cp.dx);
		next.axes[SDL_CONTROLLER_AXIS_LEFTY] = -scaleAxis(cp.dy); // SDL: down is positive
		controllerState = next; // read by the RGSS thread in Input::update
		gspWaitForVBlank();
	}
	platform3ds::removeAptHook();
}

void EventThread::cleanup() {}

void EventThread::requestFullscreenMode(bool) {}
void EventThread::requestWindowResize(int, int) {}
void EventThread::requestWindowReposition(int, int) {}
void EventThread::requestWindowCenter() {}
void EventThread::requestWindowRename(const char *) {}
void EventThread::requestShowCursor(bool) {}
void EventThread::requestTextInputMode(bool) {}
void EventThread::requestSettingsMenu() {}
void EventThread::requestTerminate() { shState->rtData().rqTerm.set(); }

bool EventThread::getFullscreen() const { return true; }
bool EventThread::getShowCursor() const { return false; }
bool EventThread::getControllerConnected() const { return true; }
SDL_GameController *EventThread::controller() const { return nullptr; }

// RGSS `print`/`msgbox` and uncaught Ruby errors: logged to stderr (3dslink or the stderr file),
// printed on the bottom-screen console and kept for main()'s exit screen. Unlike upstream it does
// not block until dismissed: a blocking box needs the renderer (PLAN §3.1 "Safety").
static SDL_SpinLock messageLock = 0;
static std::string lastMessageText;

void EventThread::showMessageBox(const char *body, int) {
	fprintf(stderr, "[msgbox] %s\n", body);
	printf("%s\n", body);
	SDL_AtomicLock(&messageLock);
	lastMessageText = body;
	SDL_AtomicUnlock(&messageLock);
}

std::string platform3ds::lastMessage() {
	SDL_AtomicLock(&messageLock);
	std::string text = lastMessageText;
	SDL_AtomicUnlock(&messageLock);
	return text;
}

// APT hook: HOME menu / sleep pause the RGSS thread: haltThreads waits until it reaches its next
// Graphics.update and parks it there until resumed (as upstream does when the window goes to the
// background). Long script work without Graphics.update delays the suspend accordingly. Nothing
// to halt once the RGSS thread has finished.
static aptHookCookie aptCookie;
static bool aptHooked;

static void aptHookFun(APT_HookType hook, void *param) {
	RGSSThreadData *rtData = static_cast<RGSSThreadData *>(param);
	switch (hook) {
	case APTHOOK_ONSUSPEND:
	case APTHOOK_ONSLEEP:
		if (!rtData->rqTermAck) rtData->syncPoint.haltThreads();
		break;
	case APTHOOK_ONRESTORE:
	case APTHOOK_ONWAKEUP: rtData->syncPoint.resumeThreads(); break;
	case APTHOOK_ONEXIT: rtData->syncPoint.resumeThreads(); break; // let it see rqTerm
	default: break;
	}
}

void platform3ds::installAptHook(RGSSThreadData &rtData) {
	aptHook(&aptCookie, aptHookFun, &rtData);
	aptHooked = true;
}

void platform3ds::removeAptHook() {
	if (aptHooked) aptUnhook(&aptCookie);
	aptHooked = false;
}

void EventThread::notifyFrame() {}
void EventThread::notifyGameScreenChange(const SDL_Rect &) {}

void EventThread::lockText(bool lock) {
	lock ? SDL_LockMutex(textInputLock) : SDL_UnlockMutex(textInputLock);
}

// SyncPoint: unchanged from upstream src/eventthread.cpp (mkxp-z, GPL-2.0-or-later). The APT
// hook above halts the RGSS thread at its next Graphics.update while the HOME menu or sleep mode
// owns the console and resumes it afterwards.
void SyncPoint::haltThreads() {
	if (mainSync.locked) return;
	reply.lock(); // lock the reply sync first to avoid races
	mainSync.lock();
	reply.waitForUnlock(); // until the RGSS thread is asleep
	secondSync.lock();
}

void SyncPoint::resumeThreads() {
	if (!mainSync.locked) return;
	mainSync.unlock(false);
	secondSync.unlock(true);
}

bool SyncPoint::mainSyncLocked() { return mainSync.locked; }

void SyncPoint::waitMainSync() {
	reply.unlock(false);
	mainSync.waitForUnlock();
}

void SyncPoint::passSecondarySync() {
	if (!secondSync.locked) return;
	secondSync.waitForUnlock();
}

SyncPoint::Util::Util() {
	mut = SDL_CreateMutex();
	cond = SDL_CreateCond();
}

SyncPoint::Util::~Util() {
	SDL_DestroyCond(cond);
	SDL_DestroyMutex(mut);
}

void SyncPoint::Util::lock() { locked.set(); }

void SyncPoint::Util::unlock(bool multi) {
	locked.clear();
	if (multi)
		SDL_CondBroadcast(cond);
	else
		SDL_CondSignal(cond);
}

void SyncPoint::Util::waitForUnlock() {
	SDL_LockMutex(mut);
	while (locked) SDL_CondWait(cond, mut);
	SDL_UnlockMutex(mut);
}
