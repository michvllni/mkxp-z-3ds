// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: Audio for the Nintendo 3DS on ndsp (replaces src/audio/*.cpp, which need OpenAL).
//
// - BGM, BGS and ME are streamed: one ndsp channel each with STREAM_BUFS wave buffers that an
//   audio thread refills from the decoder (Ogg Vorbis through Tremor, PCM WAV). The thread runs
//   on the New 3DS core 2 if the process may use it, else on core 1, else on the app core.
// - SE are decoded once into a cache (linear memory, least recently used out) and played on
//   SE_CHANNELS channels; the oldest sound gives way when all are busy (as upstream).
// - Semantics follow upstream src/audio: same file and pitch again only changes the volume,
//   pitch 50..150 %, fade out over the given milliseconds, a 1 s fade in when a position is
//   given, the ME pauses the BGM (200 ms fade out, 1 s fade in afterwards), Vorbis
//   LOOPSTART/LOOPLENGTH comments, bgm_pos in seconds. Loop comments differ on purpose:
//   LOOPSTART=0 with LOOPLENGTH loops 0..LOOPLENGTH (upstream loops the whole file; the
//   converter writes it for MIDI so the reverb tail stays outside the loop), and LOOPSTART
//   alone loops from there to the end of the file.
// - MIDI is not played on the 3DS: rmxp3ds-convert renders it to Ogg Vorbis on the PC. A file
//   in a format the runtime cannot play is skipped silently (one line on stderr); a missing file
//   raises like upstream.
// - Needs the DSP firmware sdmc:/3ds/dspfirm.cdc; without it the game runs silently.
#include "audio.h"

#include <3ds.h>
#include <SDL_rwops.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <tremor/ivorbisfile.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "exception.h"
#include "filesystem.h"
#include "sharedstate.h"

namespace {

const int STREAM_BUFS = 4;
const int STREAM_FRAMES = 4096;              // per wave buffer: 62 ms at 65456 Hz
const int CH_BGM = 0, CH_BGS = 1, CH_ME = 2; // ndsp channels
const int CH_SE = 8, SE_CHANNELS = 8;        // SE on channels 8..15
const size_t SE_CACHE_BYTES = 3u << 20;      // decoded SE kept in linear memory
const size_t SE_MAX_BYTES = 2u << 20;        // longer SE are not played (a BGM in disguise)
const int ME_FADE_OUT_MS = 200, FADE_IN_MS = 1000; // as upstream AudioPrivate / AudioStream

// --- decoders ------------------------------------------------------------------------------

struct Decoder {
	int channels = 2;
	float rate = 44100;
	int64_t loopStart = -1, loopEnd = -1; // frames (Vorbis LOOPSTART/LOOPLENGTH), -1: none
	virtual ~Decoder() {}
	// Reads up to `frames` interleaved 16-bit frames; returns the count, 0 at the end, < 0 on errors.
	virtual long read(int16_t *out, long frames) = 0;
	virtual bool seek(int64_t frame) = 0;
	virtual int64_t tell() = 0;
};

size_t rwRead(void *ptr, size_t size, size_t n, void *ds) {
	return SDL_RWread(static_cast<SDL_RWops *>(ds), ptr, size, n);
}
int rwSeek(void *ds, ogg_int64_t off, int whence) {
	return SDL_RWseek(static_cast<SDL_RWops *>(ds), off, whence) < 0 ? -1 : 0;
}
int rwClose(void *ds) {
	SDL_RWops *ops = static_cast<SDL_RWops *>(ds);
	SDL_RWclose(ops);
	delete ops;
	return 0;
}
long rwTell(void *ds) { return (long)SDL_RWtell(static_cast<SDL_RWops *>(ds)); }

struct VorbisDecoder : Decoder {
	OggVorbis_File vf;

	// Takes over ops (closed on failure too). Returns nullptr if it is not Ogg Vorbis.
	static Decoder *open(SDL_RWops &ops) {
		SDL_RWops *src = new SDL_RWops(ops);
		VorbisDecoder *d = new VorbisDecoder;
		ov_callbacks cb = {rwRead, rwSeek, rwClose, rwTell};
		if (ov_open_callbacks(src, &d->vf, nullptr, 0, cb) != 0) {
			delete d;
			rwClose(src);
			return nullptr;
		}
		vorbis_info *info = ov_info(&d->vf, -1);
		if (!info || info->channels < 1 || info->channels > 2) {
			ov_clear(&d->vf); // closes src
			delete d;
			return nullptr;
		}
		d->channels = info->channels;
		d->rate = info->rate;
		int64_t start = -1, length = -1;
		vorbis_comment *vc = ov_comment(&d->vf, -1);
		for (int i = 0; vc && i < vc->comments; ++i) {
			const char *c = vc->user_comments[i];
			if (!strncasecmp(c, "LOOPSTART=", 10)) start = strtoll(c + 10, nullptr, 10);
			if (!strncasecmp(c, "LOOPLENGTH=", 11)) length = strtoll(c + 11, nullptr, 10);
		}
		if (start >= 0) {
			d->loopStart = start;
			if (length > 0) d->loopEnd = start + length;
		}
		return d;
	}
	~VorbisDecoder() { ov_clear(&vf); }

	long read(int16_t *out, long frames) override {
		long bytes = frames * channels * 2, got = 0;
		int bs = 0;
		while (got < bytes) {
			long n = ov_read(&vf, reinterpret_cast<char *>(out) + got, (int)(bytes - got), &bs);
			if (n == OV_HOLE) continue;
			if (n < 0) return got ? got / (channels * 2) : -1;
			if (n == 0) break;
			got += n;
		}
		return got / (channels * 2);
	}
	bool seek(int64_t frame) override { return ov_pcm_seek(&vf, frame) == 0; }
	int64_t tell() override { return ov_pcm_tell(&vf); }
};

struct WavDecoder : Decoder {
	SDL_RWops ops;
	int64_t dataPos = 0, dataFrames = 0, pos = 0;
	int bits = 16;
	std::vector<uint8_t> tmp;

	static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
	static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

	// Takes over ops (closed on failure too). PCM 8/16 bit, mono or stereo.
	static Decoder *open(SDL_RWops &src) {
		WavDecoder *d = new WavDecoder;
		d->ops = src;
		uint8_t h[12];
		bool fmt = false, ok = SDL_RWread(&d->ops, h, 1, 12) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4);
		while (ok) {
			uint8_t c[8];
			if (SDL_RWread(&d->ops, c, 1, 8) != 8) { ok = false; break; }
			uint32_t size = le32(c + 4);
			int64_t next = SDL_RWtell(&d->ops) + size + (size & 1);
			if (!memcmp(c, "fmt ", 4) && size >= 16) {
				uint8_t f[16];
				if (SDL_RWread(&d->ops, f, 1, 16) != 16) { ok = false; break; }
				uint16_t tag = le16(f);
				d->channels = le16(f + 2);
				d->rate = le32(f + 4);
				d->bits = le16(f + 14);
				fmt = (tag == 1 || tag == 0xFFFE) && (d->bits == 8 || d->bits == 16) &&
				      d->channels >= 1 && d->channels <= 2 && d->rate > 0;
				if (!fmt) { ok = false; break; }
			} else if (!memcmp(c, "data", 4)) {
				if (!fmt) { ok = false; break; }
				d->dataPos = SDL_RWtell(&d->ops);
				d->dataFrames = size / (d->channels * (d->bits / 8));
				return d;
			}
			if (SDL_RWseek(&d->ops, next, RW_SEEK_SET) < 0) ok = false;
		}
		delete d; // closes ops
		return nullptr;
	}
	~WavDecoder() { SDL_RWclose(&ops); }

	long read(int16_t *out, long frames) override {
		frames = (long)std::min<int64_t>(frames, dataFrames - pos);
		if (frames <= 0) return 0;
		size_t n;
		if (bits == 16) {
			n = SDL_RWread(&ops, out, channels * 2, frames);
		} else {
			tmp.resize(frames * channels);
			n = SDL_RWread(&ops, tmp.data(), channels, frames);
			for (size_t i = 0; i < n * channels; ++i) out[i] = (int16_t)((tmp[i] - 128) << 8);
		}
		pos += n;
		return (long)n;
	}
	bool seek(int64_t frame) override {
		frame = std::max<int64_t>(0, std::min(frame, dataFrames));
		if (SDL_RWseek(&ops, dataPos + frame * channels * (bits / 8), RW_SEEK_SET) < 0) return false;
		pos = frame;
		return true;
	}
	int64_t tell() override { return pos; }
};

// Opens the first file named `filename` (any extension, as upstream) that the runtime can play.
struct DecoderOpener : FileSystem::OpenHandler {
	Decoder *dec = nullptr;
	std::string skippedExt;

	bool tryRead(SDL_RWops &ops, const char *ext) override {
		char sig[4] = {};
		SDL_RWread(&ops, sig, 1, 4);
		SDL_RWseek(&ops, 0, RW_SEEK_SET);
		if (!memcmp(sig, "OggS", 4)) dec = VorbisDecoder::open(ops);
		else if (!memcmp(sig, "RIFF", 4)) dec = WavDecoder::open(ops);
		else SDL_RWclose(&ops);
		if (dec) return true;
		skippedExt = ext ? ext : "?";
		return false;
	}
};

// Without ndsp: only checks that the file exists (throws like upstream if not); a name is
// checked once.
void checkExists(const std::string &filename) {
	static std::set<std::string> seen;
	if (seen.count(filename)) return;
	struct Closer : FileSystem::OpenHandler {
		bool tryRead(SDL_RWops &ops, const char *) override {
			SDL_RWclose(&ops);
			return true;
		}
	} closer;
	shState->fileSystem().openRead(closer, filename.c_str());
	seen.insert(filename);
}

// Throws (like upstream) if there is no such file; nullptr if none of them can be played.
Decoder *openDecoder(const std::string &filename) {
	DecoderOpener opener;
	shState->fileSystem().openRead(opener, filename.c_str());
	if (!opener.dec) {
		static std::set<std::string> told;
		if (told.insert(filename).second)
			fprintf(stderr, "audio: %s: format not playable on the 3DS (.%s), skipped\n", filename.c_str(),
			        opener.skippedExt.c_str());
	}
	return opener.dec;
}

float clampPitch(int pitch) { return std::max(50, std::min(150, pitch)) / 100.0f; }
float clampVolume(int volume) { return std::max(0, std::min(100, volume)) / 100.0f; }

void setMix(int ch, float vol) {
	float mix[12] = {};
	mix[0] = mix[1] = vol;
	ndspChnSetMix(ch, mix);
}

// --- streams -------------------------------------------------------------------------------

struct Stream {
	int ch;
	bool looped;
	Decoder *dec = nullptr;
	std::string file;
	float volume = 1, pitch = 1;
	float group = 1;   // bgm_volume (BGM only)
	float duck = 1;    // ME watch (BGM only)
	float fadeOut = 1, fadeIn = 1;
	u64 fadeOutStart = 0, fadeInStart = 0;
	int fadeOutMs = 0;
	bool fadingOut = false, fadingIn = false;
	bool active = false; // started and not yet finished or stopped
	bool paused = false; // by the ME watch
	bool deferred = false; // started while an ME played: plays at full volume when it ends
	bool ended = false;  // decoder exhausted (not looped): finishes when the queue is empty
	ndspWaveBuf wb[STREAM_BUFS];
	int16_t *data[STREAM_BUFS] = {};
	int64_t bufStart[STREAM_BUFS] = {}; // file frame of each buffer's first sample
	u64 bufOrder[STREAM_BUFS] = {};     // queue order (oldest first)
	u64 queued = 0;

	Stream(int ch, bool looped) : ch(ch), looped(looped) {}

	bool alloc() {
		for (int i = 0; i < STREAM_BUFS; ++i) {
			data[i] = static_cast<int16_t *>(linearAlloc(STREAM_FRAMES * 2 * sizeof(int16_t)));
			if (!data[i]) return false;
		}
		return true;
	}
	void free() {
		stop();
		for (int i = 0; i < STREAM_BUFS; ++i) {
			if (data[i]) linearFree(data[i]);
			data[i] = nullptr;
		}
	}

	void applyVolume() { setMix(ch, volume * group * duck * fadeOut * fadeIn); }

	void stop() {
		ndspChnWaveBufClear(ch);
		delete dec;
		dec = nullptr;
		active = paused = ended = fadingOut = fadingIn = deferred = false;
		fadeOut = fadeIn = 1;
		file.clear();
	}

	// Starts `d` (taken over) at `pos` seconds.
	void start(Decoder *d, const std::string &name, float vol, float pit, double pos, bool startPaused) {
		stop();
		dec = d;
		file = name;
		volume = vol;
		pitch = pit;
		ndspChnReset(ch);
		ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
		ndspChnSetFormat(ch, dec->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
		ndspChnSetRate(ch, dec->rate * pitch);
		if (pos > 0) {
			dec->seek((int64_t)(pos * dec->rate));
			fadeIn = 0;
			fadingIn = true;
			fadeInStart = osGetTime();
		}
		memset(wb, 0, sizeof(wb));
		active = true;
		paused = startPaused;
		ndspChnSetPaused(ch, paused);
		applyVolume();
		fill();
	}

	// Queues decoded audio into every free wave buffer.
	void fill() {
		if (!active || !dec) return;
		for (int i = 0; i < STREAM_BUFS && !ended; ++i) {
			if (wb[i].status != NDSP_WBUF_FREE && wb[i].status != NDSP_WBUF_DONE) continue;
			long got = 0;
			bool rewound = false;
			bufStart[i] = -1;
			while (got < STREAM_FRAMES) {
				long want = STREAM_FRAMES - got;
				if (looped && dec->loopEnd > 0) want = (long)std::min<int64_t>(want, dec->loopEnd - dec->tell());
				long n = want > 0 ? dec->read(data[i] + got * dec->channels, want) : 0;
				if (n > 0) {
					if (bufStart[i] < 0) bufStart[i] = dec->tell() - n; // after a loop jump: the jump target
					got += n;
					rewound = false;
					continue;
				}
				// End of the file, of the loop, or an error: loop back or finish. The end of the file
				// becomes the loop end if none (or one past it) was given, so bgm_pos wraps there.
				if (n == 0 && want > 0 && looped && !rewound && (dec->loopEnd <= 0 || dec->loopEnd > dec->tell()))
					dec->loopEnd = dec->tell();
				if (n < 0 || !looped || rewound || !dec->seek(dec->loopStart > 0 ? dec->loopStart : 0)) {
					ended = true;
					break;
				}
				rewound = true;
			}
			if (got == 0) break;
			wb[i].data_vaddr = data[i];
			wb[i].nsamples = got;
			wb[i].looping = false;
			bufOrder[i] = ++queued;
			DSP_FlushDataCache(data[i], got * dec->channels * sizeof(int16_t));
			ndspChnWaveBufAdd(ch, &wb[i]);
		}
	}

	bool queueEmpty() const {
		for (int i = 0; i < STREAM_BUFS; ++i)
			if (wb[i].status == NDSP_WBUF_QUEUED || wb[i].status == NDSP_WBUF_PLAYING) return false;
		return true;
	}

	// Per tick: fades, end of the file.
	void tick(u64 now) {
		if (!active) return;
		if (fadingIn) {
			fadeIn = std::min(1.0f, (now - fadeInStart) / (float)FADE_IN_MS);
			if (fadeIn >= 1) fadingIn = false;
		}
		if (fadingOut) {
			fadeOut = 1.0f - (now - fadeOutStart) / (float)fadeOutMs;
			if (fadeOut <= 0) {
				stop();
				return;
			}
		}
		if (!paused) fill();
		if (ended && queueEmpty()) {
			stop();
			return;
		}
		applyVolume();
	}

	void fade(int ms) {
		if (!active || fadingOut) return;
		if (paused) {
			stop();
			return;
		}
		fadingOut = true;
		fadeOutMs = std::max(1, ms);
		fadeOutStart = osGetTime();
	}

	// Seconds into the file of the sample playing now: the playing buffer's start plus the DSP's
	// position in it (else the oldest queued buffer's start), wrapped into the loop.
	double position() const {
		if (!active || !dec) return 0;
		int64_t pos = -1;
		for (int i = 0; i < STREAM_BUFS && pos < 0; ++i)
			if (wb[i].status == NDSP_WBUF_PLAYING) pos = bufStart[i] + ndspChnGetSamplePos(ch);
		if (pos < 0) {
			int oldest = -1;
			for (int i = 0; i < STREAM_BUFS; ++i)
				if (wb[i].status == NDSP_WBUF_QUEUED && (oldest < 0 || bufOrder[i] < bufOrder[oldest])) oldest = i;
			pos = oldest >= 0 ? bufStart[oldest] : dec->tell();
		}
		int64_t ls = dec->loopStart > 0 ? dec->loopStart : 0;
		if (looped && dec->loopEnd > ls && pos >= dec->loopEnd) pos = ls + (pos - ls) % (dec->loopEnd - ls);
		return std::max<int64_t>(0, pos) / dec->rate;
	}
};

// --- sound effects -------------------------------------------------------------------------

struct SEBuffer {
	std::string name;
	int16_t *pcm = nullptr; // linear memory
	u32 frames = 0;
	int channels = 1;
	float rate = 44100;
	size_t bytes = 0;
	u64 lastUse = 0;
};

} // namespace

struct AudioPrivate {
	bool ready = false;
	int bgmVolume = 100;
	LightLock lock;
	LightEvent wake;
	Thread thread = nullptr;
	volatile bool quit = false;
	int core = -2;

	Stream bgm{CH_BGM, true}, bgs{CH_BGS, true}, me{CH_ME, false};

	enum MeState { MeNotPlaying, BgmFadingOut, MePlaying, BgmFadingIn } meState = MeNotPlaying;
	u64 meTick = 0;

	std::vector<SEBuffer *> seCache;
	size_t seBytes = 0;
	SEBuffer *seOn[SE_CHANNELS] = {};
	ndspWaveBuf seWb[SE_CHANNELS] = {};
	u64 seStarted[SE_CHANNELS] = {};
	u64 seClock = 0;

	static void frameCallback(void *data) { LightEvent_Signal(&static_cast<AudioPrivate *>(data)->wake); }

	static void threadFun(void *arg) {
		AudioPrivate *p = static_cast<AudioPrivate *>(arg);
		while (!p->quit) {
			LightEvent_WaitTimeout(&p->wake, 20 * 1000 * 1000LL);
			LightLock_Lock(&p->lock);
			p->update();
			LightLock_Unlock(&p->lock);
		}
	}

	AudioPrivate() {
		LightLock_Init(&lock);
		LightEvent_Init(&wake, RESET_ONESHOT);
		Result rc = ndspInit();
		if (R_FAILED(rc)) {
			fprintf(stderr, "audio: ndspInit failed (0x%08lx; is sdmc:/3ds/dspfirm.cdc there?), no sound\n", rc);
			return;
		}
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		if (!bgm.alloc() || !bgs.alloc() || !me.alloc()) {
			fprintf(stderr, "audio: no linear memory for stream buffers, no sound\n");
			freeAll();
			ndspExit();
			return;
		}
		ndspSetCallback(frameCallback, this);
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		prio = std::max<s32>(0x18, prio - 1);
		// New 3DS core 2, else the system core (needs a CPU time limit), else the app core.
		thread = threadCreate(threadFun, this, 32 * 1024, prio, core = 2, false);
		if (!thread && R_SUCCEEDED(APT_SetAppCpuTimeLimit(30)))
			thread = threadCreate(threadFun, this, 32 * 1024, prio, core = 1, false);
		if (!thread) thread = threadCreate(threadFun, this, 32 * 1024, prio, core = -2, false);
		if (!thread) {
			fprintf(stderr, "audio: cannot create the audio thread, no sound\n");
			ndspSetCallback(nullptr, nullptr);
			freeAll();
			ndspExit();
			return;
		}
		ready = true;
		fprintf(stderr, "audio: ndsp ready, audio thread on core %d\n", core);
	}

	~AudioPrivate() {
		if (!ready) return;
		quit = true;
		LightEvent_Signal(&wake);
		threadJoin(thread, U64_MAX);
		threadFree(thread);
		ndspSetCallback(nullptr, nullptr);
		freeAll();
		ndspExit();
	}

	void freeAll() {
		bgm.free();
		bgs.free();
		me.free();
		seStopLocked();
		for (SEBuffer *b : seCache) {
			linearFree(b->pcm);
			delete b;
		}
		seCache.clear();
		seBytes = 0;
	}

	// Audio thread, lock held.
	void update() {
		u64 now = osGetTime();
		bgm.tick(now);
		bgs.tick(now);
		me.tick(now);
		meWatch(now);
	}

	// The ME pauses the BGM: as upstream AudioPrivate::meWatchFun.
	void meWatch(u64 now) {
		float dt = (float)(now - meTick);
		meTick = now;
		switch (meState) {
		case MeNotPlaying:
			if (me.active) meState = BgmFadingOut;
			break;
		case BgmFadingOut:
			if (!me.active) {
				meState = BgmFadingIn;
				break;
			}
			bgm.duck -= dt / ME_FADE_OUT_MS;
			if (bgm.duck <= 0 || !bgm.active) {
				bgm.duck = 0;
				if (bgm.active) {
					bgm.paused = true;
					ndspChnSetPaused(bgm.ch, true);
				}
				meState = MePlaying;
			}
			bgm.applyVolume();
			break;
		case MePlaying:
			if (!me.active) {
				if (bgm.active && bgm.paused && bgm.deferred) {
					// Started during the ME: as upstream (extPaused), full volume at once.
					bgm.paused = bgm.deferred = false;
					ndspChnSetPaused(bgm.ch, false);
					bgm.duck = 1;
					bgm.applyVolume();
					meState = MeNotPlaying;
				} else if (bgm.active && bgm.paused) {
					bgm.paused = false;
					ndspChnSetPaused(bgm.ch, false);
					meState = BgmFadingIn;
				} else {
					// No BGM was held: as upstream, full volume at once.
					bgm.duck = 1;
					bgm.applyVolume();
					meState = MeNotPlaying;
				}
			}
			break;
		case BgmFadingIn:
			if (me.active) {
				meState = BgmFadingOut;
				break;
			}
			if (!bgm.active) { // stopped meanwhile: as upstream, no fade for the next one
				bgm.duck = 1;
				meState = MeNotPlaying;
				break;
			}
			bgm.duck += dt / FADE_IN_MS;
			if (bgm.duck >= 1) {
				bgm.duck = 1;
				meState = MeNotPlaying;
			}
			if (bgm.active && bgm.paused) {
				bgm.paused = false;
				ndspChnSetPaused(bgm.ch, false);
			}
			bgm.applyVolume();
			break;
		}
	}

	// Upstream AudioStream::play: the same file and pitch again only changes the volume.
	void play(Stream &s, const char *filename, int volume, int pitch, double pos) {
		std::string name = filename ? filename : "";
		if (!ready) return checkExists(name);
		float vol = clampVolume(volume), pit = clampPitch(pitch);
		LightLock_Lock(&lock);
		if (s.active && !s.fadingOut && name == s.file && pit == s.pitch) {
			s.volume = vol;
			s.applyVolume();
			LightLock_Unlock(&lock);
			return;
		}
		LightLock_Unlock(&lock);
		Decoder *d = openDecoder(name); // file I/O outside the lock; throws if missing
		LightLock_Lock(&lock);
		if (d) {
			bool held = &s == &bgm && (meState == BgmFadingOut || meState == MePlaying);
			s.start(d, name, vol, pit, pos, held);
			if (held) {
				bgm.duck = 0;
				bgm.deferred = true;
			}
		} else {
			s.stop();
		}
		LightLock_Unlock(&lock);
	}

	void stop(Stream &s) {
		if (!ready) return;
		LightLock_Lock(&lock);
		s.stop();
		LightLock_Unlock(&lock);
	}

	void fade(Stream &s, int ms) {
		if (!ready) return;
		LightLock_Lock(&lock);
		s.fade(ms);
		LightLock_Unlock(&lock);
	}

	double pos(Stream &s) {
		if (!ready) return 0;
		LightLock_Lock(&lock);
		double r = s.position();
		LightLock_Unlock(&lock);
		return r;
	}

	// --- SE ---

	// Busy: its wave buffer is queued or playing. The status is set at once by
	// ndspChnWaveBufAdd; ndspChnIsPlaying only follows the DSP's state once per DSP frame, so two
	// SE in the same Ruby frame would share a channel and a queued one could be evicted.
	bool seBusy(int k) const {
		return seWb[k].status == NDSP_WBUF_QUEUED || seWb[k].status == NDSP_WBUF_PLAYING ||
		       ndspChnIsPlaying(CH_SE + k);
	}

	bool seInUse(SEBuffer *b) {
		for (int i = 0; i < SE_CHANNELS; ++i)
			if (seOn[i] == b && seBusy(i)) return true;
		return false;
	}

	void seStopLocked() {
		for (int i = 0; i < SE_CHANNELS; ++i) {
			ndspChnWaveBufClear(CH_SE + i);
			seWb[i].status = NDSP_WBUF_FREE; // detached: not busy any more (seBusy)
			seOn[i] = nullptr;
		}
	}

	// Frees least recently used buffers that do not play until `need` more bytes fit.
	void seEvict(size_t need) {
		std::sort(seCache.begin(), seCache.end(), [](SEBuffer *a, SEBuffer *b) { return a->lastUse < b->lastUse; });
		for (size_t i = 0; i < seCache.size() && seBytes + need > SE_CACHE_BYTES;) {
			SEBuffer *b = seCache[i];
			if (seInUse(b)) {
				++i;
				continue;
			}
			for (int k = 0; k < SE_CHANNELS; ++k)
				if (seOn[k] == b) seOn[k] = nullptr;
			seBytes -= b->bytes;
			linearFree(b->pcm);
			delete b;
			seCache.erase(seCache.begin() + i);
		}
	}

	// Decodes a whole SE into linear memory (Ruby thread, outside the lock). nullptr if it
	// cannot be played or there is no linear memory; throws if the file is missing.
	SEBuffer *decodeSE(const std::string &name) {
		Decoder *d = openDecoder(name);
		if (!d) return nullptr;
		std::vector<int16_t> pcm;
		std::vector<int16_t> chunk(4096 * d->channels);
		long n;
		while ((n = d->read(chunk.data(), 4096)) > 0) {
			pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + n * d->channels);
			if (pcm.size() * sizeof(int16_t) > SE_MAX_BYTES) {
				fprintf(stderr, "audio: SE %s: over %u KiB decoded, skipped\n", name.c_str(),
				        (unsigned)(SE_MAX_BYTES >> 10));
				delete d;
				return nullptr;
			}
		}
		int channels = d->channels;
		float rate = d->rate;
		delete d;
		if (pcm.empty()) return nullptr;
		size_t bytes = pcm.size() * sizeof(int16_t);
		LightLock_Lock(&lock);
		seEvict(bytes);
		LightLock_Unlock(&lock);
		int16_t *mem = static_cast<int16_t *>(linearAlloc(bytes));
		if (!mem) {
			fprintf(stderr, "audio: SE %s: no linear memory, skipped\n", name.c_str());
			return nullptr;
		}
		memcpy(mem, pcm.data(), bytes);
		DSP_FlushDataCache(mem, bytes);
		SEBuffer *b = new SEBuffer;
		b->name = name;
		b->pcm = mem;
		b->channels = channels;
		b->rate = rate;
		b->frames = pcm.size() / channels;
		b->bytes = bytes;
		return b;
	}

	SEBuffer *seFind(const std::string &name) {
		for (SEBuffer *b : seCache)
			if (b->name == name) return b;
		return nullptr;
	}

	// Upstream SoundEmitter::play: a free channel, else one that plays the same sound, else
	// the one started longest ago.
	void sePlay(const char *filename, int volume, int pitch) {
		std::string name = filename ? filename : "";
		if (!ready) return checkExists(name);
		float vol = clampVolume(volume), pit = clampPitch(pitch);
		LightLock_Lock(&lock);
		SEBuffer *buf = seFind(name);
		LightLock_Unlock(&lock);
		if (!buf) {
			SEBuffer *fresh = decodeSE(name);
			if (!fresh) return;
			LightLock_Lock(&lock);
			buf = seFind(name); // decoded meanwhile by nobody else (one Ruby thread), but be safe
			if (buf) {
				linearFree(fresh->pcm);
				delete fresh;
			} else {
				buf = fresh;
				seCache.push_back(buf);
				seBytes += buf->bytes;
			}
			LightLock_Unlock(&lock);
		}
		LightLock_Lock(&lock);
		int k = -1;
		for (int i = 0; i < SE_CHANNELS && k < 0; ++i)
			if (!seBusy(i)) k = i;
		for (int i = 0; i < SE_CHANNELS && k < 0; ++i)
			if (seOn[i] == buf) k = i;
		if (k < 0) {
			k = 0;
			for (int i = 1; i < SE_CHANNELS; ++i)
				if (seStarted[i] < seStarted[k]) k = i;
		}
		int ch = CH_SE + k;
		ndspChnWaveBufClear(ch);
		ndspChnReset(ch);
		ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
		ndspChnSetFormat(ch, buf->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
		ndspChnSetRate(ch, buf->rate * pit);
		setMix(ch, vol);
		memset(&seWb[k], 0, sizeof(seWb[k]));
		seWb[k].data_vaddr = buf->pcm;
		seWb[k].nsamples = buf->frames;
		ndspChnWaveBufAdd(ch, &seWb[k]);
		seOn[k] = buf;
		seStarted[k] = ++seClock;
		buf->lastUse = seClock;
		LightLock_Unlock(&lock);
	}

	void reset() {
		if (!ready) return;
		LightLock_Lock(&lock);
		bgm.stop();
		bgs.stop();
		me.stop();
		bgm.duck = 1;
		meState = MeNotPlaying;
		seStopLocked();
		LightLock_Unlock(&lock);
	}
};

Audio::Audio(RGSSThreadData &) : p(new AudioPrivate) {}
Audio::~Audio() { delete p; }

void Audio::bgmPlay(const char *filename, int volume, int pitch, double pos, int) {
	p->play(p->bgm, filename, volume, pitch, pos);
}
void Audio::bgmStop(int) { p->stop(p->bgm); }
void Audio::bgmFade(int time, int) { p->fade(p->bgm, time); }
int Audio::bgmGetVolume(int) { return p->bgmVolume; }
void Audio::bgmSetVolume(int volume, int) {
	p->bgmVolume = std::max(0, std::min(100, volume));
	if (!p->ready) return;
	LightLock_Lock(&p->lock);
	p->bgm.group = p->bgmVolume / 100.0f;
	p->bgm.applyVolume();
	LightLock_Unlock(&p->lock);
}
void Audio::bgsPlay(const char *filename, int volume, int pitch, double pos) {
	p->play(p->bgs, filename, volume, pitch, pos);
}
void Audio::bgsStop() { p->stop(p->bgs); }
void Audio::bgsFade(int time) { p->fade(p->bgs, time); }
void Audio::mePlay(const char *filename, int volume, int pitch) { p->play(p->me, filename, volume, pitch, 0); }
void Audio::meStop() { p->stop(p->me); }
void Audio::meFade(int time) { p->fade(p->me, time); }
void Audio::sePlay(const char *filename, int volume, int pitch) { p->sePlay(filename, volume, pitch); }
void Audio::seStop() {
	if (!p->ready) return;
	LightLock_Lock(&p->lock);
	p->seStopLocked();
	LightLock_Unlock(&p->lock);
}
void Audio::setupMidi() {}
double Audio::bgmPos(int) { return p->pos(p->bgm); }
double Audio::bgsPos() { return p->pos(p->bgs); }
void Audio::reset() { p->reset(); }
