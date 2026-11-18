// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: program entry for the Nintendo 3DS (replaces src/main.cpp in the 3DS build).
//
// Layout as upstream: the main thread reads the configuration, starts the RGSS thread (Ruby,
// game scripts, graphics) and then runs the event loop (EventThread::process: buttons, APT).
// Differences:
// - The RGSS thread is a libctru thread with a 1 MiB stack (Ruby needs about 950 KiB, rmxp3ds
//   NOTES "Hardware run 1"); its size is passed to Ruby (rb3ds_main_stack_size) for the stack
//   overflow check. It runs one priority level below the main thread on the same core, so input
//   is read as soon as a vertical blank arrives.
// - The game directory is argv[1] if given (3dslink / homebrew launcher arguments), otherwise
//   sdmc:/rmxp3ds/game. Config (mkxp.json) and Game.ini are read from there, as upstream.
// - No window, OpenGL context or OpenAL device: the GL function table is the 3DS one (gl3ds.cpp).
// - Without 3dslink, stderr goes to sdmc:/rmxp3ds/mkxp-stderr.txt (Ruby errors, Debug() output).
#define SDL_MAIN_HANDLED // our own main(); SDL_main.h would rename it
#include <3ds.h>
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "binding.h"
#include "config.h"
#include "debugwriter.h"
#include "eventthread.h"
#include "exception.h"
#include "gl-fun.h"
#include "keybindings.h"
#include "rb3ds.h"
#include "sharedstate.h"

// libctru: size of the main thread's stack (the event loop needs little).
extern "C" {
u32 __stacksize__ = 256 * 1024;
}

namespace {
const size_t RGSS_STACK_SIZE = 1024 * 1024;
const char *const DEFAULT_GAME_DIR = "sdmc:/rmxp3ds/game";
const char *const STDERR_FILE = "sdmc:/rmxp3ds/mkxp-stderr.txt";
const u32 SOC_BUFFER_SIZE = 0x100000;

// Starts the 3dslink stderr redirection if the app was started by 3dslink. Returns the socket
// buffer to free later, or nullptr.
u32 *connect3dslink() {
	if (__3dslink_host.s_addr == 0) return nullptr;
	u32 *buf = (u32 *)memalign(0x1000, SOC_BUFFER_SIZE);
	if (buf && R_SUCCEEDED(socInit(buf, SOC_BUFFER_SIZE))) {
		link3dsStdioForDebug();
		return buf;
	}
	free(buf);
	return nullptr;
}

// RGSS thread: GL table, shared state (filesystem, graphics, input, audio, fonts), then the
// script binding (Ruby). Errors before the scripts run end up in rgssErrorMsg.
void rgssThreadFun(void *userdata) {
	RGSSThreadData *threadData = static_cast<RGSSThreadData *>(userdata);
	rb3ds_main_stack_size = RGSS_STACK_SIZE;
	initGLFunctions();
	try {
		SharedState::initInstance(threadData);
	} catch (const Exception &exc) {
		threadData->rgssErrorMsg = exc.msg;
		threadData->rqTermAck.set();
		return;
	}
	scriptBinding->execute();
	threadData->rqTermAck.set();
	SharedState::finiInstance();
}
} // namespace

int main(int argc, char *argv[]) {
	osSetSpeedupEnable(true);
	gfxInitDefault();
	PrintConsole bottom;
	consoleInit(GFX_BOTTOM, &bottom);
	mkdir("sdmc:/rmxp3ds", 0777);
	u32 *socBuffer = connect3dslink();
	if (!socBuffer && freopen(STDERR_FILE, "w", stderr)) setvbuf(stderr, nullptr, _IONBF, 0);

	const char *gameDir = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : DEFAULT_GAME_DIR;
	printf("mkxp-z-3ds %s\ngame: %s\n", MKXPZ_VERSION, gameDir);
	int status = 0;
	bool stuck = false;
	std::string error;
	std::string argv0; // outlives the RGSS thread (stuck case: until the process ends)
	if (chdir(gameDir) != 0) error = std::string("cannot enter the game directory ") + gameDir;

	SDL_SetMainReady();
	if (error.empty() && SDL_Init(SDL_INIT_TIMER) < 0) error = std::string("SDL_Init: ") + SDL_GetError();

	if (error.empty()) {
		// Config::read only looks at argv[1..] for "debug"/"test"/"btest"; the game directory
		// argument is handled above.
		// argv[0] must be a path: PhysFS derives its base directory from it. Loaders that start
		// the app without arguments (Azahar, some launchers) get one inside the game directory.
		argv0 = (argc > 0 && argv[0] && strchr(argv[0], '/')) ? argv[0] : std::string(gameDir) + "/mkxp-z.3dsx";
		char *configArgv[] = {&argv0[0], nullptr};
		// Heap objects: if the RGSS thread does not stop, it still uses them, so they are leaked
		// rather than destroyed under it (the process ends right after the error screen).
		Config *conf = new Config;
		try {
			conf->read(1, configArgv);
		} catch (const Exception &exc) {
			error = exc.msg;
		}
		// rmxp3ds D15: render the game screen at 3DS height. mkxp-z's Hires mode scales the screen
		// buffers by framebufferScalingFactor (bitmaps keep their size, hook in bitmap.cpp); the
		// default factor fits RGSS1's 640x480 into 240 lines, mkxp.json can set another one.
		conf->enableHires = true;
		if (conf->framebufferScalingFactor == 1.0) conf->framebufferScalingFactor = 240.0 / 480.0;
		EventThread *eventThread = nullptr;
		RGSSThreadData *rtData = nullptr;
		if (error.empty()) {
			IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
			TTF_Init();
			eventThread = new EventThread;
			rtData = new RGSSThreadData(eventThread, configArgv[0], nullptr, nullptr, 60, 1, *conf, nullptr);
			rtData->windowSizeMsg.post(Vec2i(400, 240));
			rtData->drawableSizeMsg.post(Vec2i(400, 240));
			rtData->bindingUpdateMsg.post(loadBindings(*conf));

			s32 prio = 0x30;
			svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
			Thread rgss = threadCreate(rgssThreadFun, rtData, RGSS_STACK_SIZE, prio + 1, -2, false);
			if (!rgss) {
				error = "cannot create the RGSS thread";
			} else {
				eventThread->process(*rtData);
				rtData->rqTerm.set();
				// Wait up to 10 s for the RGSS thread to acknowledge (as upstream).
				for (int i = 0; i < 1000 && !rtData->rqTermAck; ++i) svcSleepThread(10 * 1000 * 1000LL);
				if (rtData->rqTermAck) {
					threadJoin(rgss, U64_MAX);
					threadFree(rgss);
				} else {
					error = "the RGSS script seems to be stuck";
					stuck = true;
				}
				// Initialisation error or uncaught Ruby error (set in showExc)
				if (error.empty()) error = rtData->rgssErrorMsg;
			}
		}
		if (!stuck) {
			delete rtData;
			delete eventThread;
			delete conf;
			TTF_Quit();
			IMG_Quit();
			SDL_Quit();
		}
	}

	if (!error.empty()) {
		fprintf(stderr, "mkxp-z-3ds: %s\n", error.c_str());
		printf("\n%s\n\nPress START to exit.\n", error.c_str());
		bool autoexit = access("sdmc:/rmxp3ds/test/autoexit", F_OK) == 0;
		for (int frame = 0; aptMainLoop() && !(autoexit && frame > 180); ++frame) {
			hidScanInput();
			if (hidKeysDown() & KEY_START) break;
			gspWaitForVBlank();
		}
		status = 1;
	}
	// A stuck RGSS thread still uses everything below: end the process without tearing down.
	if (stuck) svcExitProcess();

	if (socBuffer) {
		socExit();
		free(socBuffer);
	}
	gfxExit();
	return status;
}
