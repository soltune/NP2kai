/* === sound management for wx port (SDL3 audio backend) ===
 * Based on sdl/soundmng.c - adapted for NP2_WX
 */

#include <soundmng.h>
#include <generic/cmver.h>

#if defined(_MSC_VER)
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif

UINT8
snddrv_drv2num(const char *cfgstr)
{
	if (strcasecmp(cfgstr, "SDL") == 0)
		return SNDDRV_SDL;
#if defined(__APPLE__)
	if (strcasecmp(cfgstr, "CoreAudio") == 0)
		return SNDDRV_COREAUDIO;
#endif
	return SNDDRV_NODRV;
}

const char *
snddrv_num2drv(UINT8 num)
{
	switch (num) {
	case SNDDRV_SDL:
		return "SDL";
#if defined(__APPLE__)
	case SNDDRV_COREAUDIO:
		return "CoreAudio";
#endif
	}
	return "nosound";
}

#if !defined(NOSOUND)

#include <np2.h>
#include <pccore.h>
#include <ini.h>
#include <dosio.h>
#include <common/parts.h>
#include <sysmng.h>
#include <sound/sound.h>

#if defined(VERMOUTH_LIB)
#include "sound/vermouth/vermouth.h"
MIDIMOD vermouth_module = NULL;
#endif

#define g_printerr(...) do { } while(0)

static struct {
	BRESULT (*drvinit)(UINT rate, UINT samples);
	BRESULT (*drvterm)(void);
	void    (*drvlock)(void);
	void    (*drvunlock)(void);
	void    (*sndplay)(void);
	void    (*sndstop)(void);
	void   *(*pcmload)(UINT num, const char *path);
	void    (*pcmdestroy)(void *cookie, UINT num);
	void    (*pcmplay)(void *cookie, UINT num, BOOL loop);
	void    (*pcmstop)(void *cookie, UINT num);
	void    (*pcmvolume)(void *cookie, UINT num, int volume);
} snddrv;

static SDL_AudioStream *audio_st = NULL;
static BOOL  opened     = FALSE;
static UINT  opna_frame = 0;

static BRESULT nosound_setup(void);
static BRESULT sdlaudio_setup(void);
#if defined(__APPLE__)
static BRESULT coreaudio_setup(void);
#endif

static void PARTSCALL (*fnmix)(SINT16 *dst, const SINT32 *src, UINT size);

/* ---- PCM channels ---- */
typedef struct {
	void *cookie;
	char *path;
	int   volume;
} pcm_channel_t;
static pcm_channel_t *pcm_channel[SOUND_MAXPCM];

static void soundmng_pcminit(void);
static void soundmng_pcmdestroy(void);

#ifndef PCM_VOULE_DEFAULT
#define PCM_VOULE_DEFAULT 25
#endif
int pcm_volume_default = PCM_VOULE_DEFAULT;

/* ---- buffer management ----
 * soundmng_sync() fills every free block (the core renders ahead as
 * needed), so the queue runs close to full: the output latency is about
 * NSOUNDBUFFER * block, i.e. the configured buffer delay. */
#ifndef NSOUNDBUFFER
#define NSOUNDBUFFER 16
#endif
static struct sndbuf {
	struct sndbuf *next;
	char          *buf;
	UINT           size;
	UINT           remain;
} sound_buffer[NSOUNDBUFFER];

static struct sndbuf *sndbuf_freelist;
#define SNDBUF_FREELIST_FIRST()           (sndbuf_freelist)
#define SNDBUF_FREELIST_INIT()            do { sndbuf_freelist = NULL; } while(0)
#define SNDBUF_FREELIST_INSERT_HEAD(s)    do { (s)->next = sndbuf_freelist; sndbuf_freelist = (s); } while(0)
#define SNDBUF_FREELIST_REMOVE_HEAD()     do { sndbuf_freelist = sndbuf_freelist->next; } while(0)

static struct {
	struct sndbuf  *first;
	struct sndbuf **last;
} sndbuf_filled;
#define SNDBUF_FILLED_QUEUE_FIRST()       (sndbuf_filled.first)
#define SNDBUF_FILLED_QUEUE_INIT()        do { sndbuf_filled.first = NULL; sndbuf_filled.last = &sndbuf_filled.first; } while(0)
#define SNDBUF_FILLED_QUEUE_INSERT_TAIL(s) do { \
	(s)->next = NULL; \
	*sndbuf_filled.last = (s); \
	sndbuf_filled.last = &(s)->next; \
} while(0)
#define SNDBUF_FILLED_QUEUE_REMOVE_HEAD() do { \
	sndbuf_filled.first = sndbuf_filled.first->next; \
	if (sndbuf_filled.first == NULL) sndbuf_filled.last = &sndbuf_filled.first; \
} while(0)

#define sndbuf_lock()     sounddrv_lock()
#define sndbuf_unlock()   sounddrv_unlock()

static BRESULT buffer_init(void);
static void    buffer_destroy(void);
static void    buffer_clear(void);


static UINT calc_blocksize(UINT size)
{
	UINT s = size;
	if (size & (size - 1))
		for (s = 32; s < size; s <<= 1)
			continue;
	return s;
}

/* ---- snddrv dispatch ---- */
static void sounddrv_lock(void)   { (*snddrv.drvlock)(); }
static void sounddrv_unlock(void) { (*snddrv.drvunlock)(); }

static void snddrv_setup(void)
{
	if (np2oscfg.snddrv < SNDDRV_DRVMAX) {
		switch (np2oscfg.snddrv) {
		case SNDDRV_SDL:
			if (sdlaudio_setup() == SUCCESS) return;
			break;
#if defined(__APPLE__)
		case SNDDRV_COREAUDIO:
			if (coreaudio_setup() == SUCCESS) return;
			break;
#endif
		}
	} else {
#if defined(__APPLE__)
		if (coreaudio_setup() == SUCCESS) {
			np2oscfg.snddrv = SNDDRV_COREAUDIO;
			sysmng_update(SYS_UPDATEOSCFG);
			return;
		}
#endif
		if (sdlaudio_setup() == SUCCESS) {
			np2oscfg.snddrv = SNDDRV_SDL;
			sysmng_update(SYS_UPDATEOSCFG);
			return;
		}
	}
	nosound_setup();
	np2oscfg.snddrv = SNDDRV_NODRV;
	sysmng_update(SYS_UPDATEOSCFG);
}

/* ---- public API ---- */

UINT soundmng_create(UINT rate, UINT bufmsec)
{
	pcm_channel_t *chan;
	UINT samples;
	int  i;

	if (opened) return 0;

	switch (rate) {
	case 11025: case 22050: case 44100: case 48000:
	case 88200: case 96000: case 176400: case 192000:
		break;
	default:
		return 0;
	}

	if (bufmsec < 10)  bufmsec = 10;
	if (bufmsec > 1000) bufmsec = 1000;

	for (i = 0; i < SOUND_MAXPCM; i++) {
		chan = pcm_channel[i];
		if (chan && chan->cookie) {
			soundmng_pcmstop(i);
			(*snddrv.pcmdestroy)(chan->cookie, i);
			chan->cookie = NULL;
		}
	}

	snddrv_setup();

	samples = (rate * bufmsec) / 1000 / NSOUNDBUFFER;
#if defined(__APPLE__)
	/* CoreAudio takes any block size; rounding up to a power of two
	 * would add up to 2x the requested delay. */
	if (np2oscfg.snddrv == SNDDRV_COREAUDIO) {
		if (samples < 32) samples = 32;
	} else
#endif
	samples = calc_blocksize(samples);
	opna_frame = samples * 2 * sizeof(SINT16);

	if ((*snddrv.drvinit)(rate, samples) != SUCCESS) {
		audio_st = NULL;
		nosound_setup();
		np2oscfg.snddrv = SNDDRV_NODRV;
		sysmng_update(SYS_UPDATEOSCFG);
		return 0;
	}

#if defined(VERMOUTH_LIB)
	vermouth_module = midimod_create(rate);
	midimod_loadall(vermouth_module);
#endif

	soundmng_setreverse(FALSE);
	buffer_init();

	for (i = 0; i < SOUND_MAXPCM; i++) {
		chan = pcm_channel[i];
		if (chan && chan->path) {
			chan->cookie = (*snddrv.pcmload)(i, chan->path);
			if (chan->cookie)
				(*snddrv.pcmvolume)(chan->cookie, i, chan->volume);
		}
	}

	opened = TRUE;
	return samples;
}

void soundmng_reset(void)
{
	sounddrv_lock();
	buffer_clear();
	sounddrv_unlock();
}

void soundmng_destroy(void)
{
	UINT i;
	if (opened) {
#if defined(VERMOUTH_LIB)
		midimod_destroy(vermouth_module);
		vermouth_module = NULL;
#endif
		for (i = 0; i < SOUND_MAXPCM; i++)
			soundmng_pcmstop(i);
		(*snddrv.sndstop)();
		(*snddrv.drvterm)();
		nosound_setup();
		audio_st = NULL;
		opened   = FALSE;
	}
}

void soundmng_play(void)  { (*snddrv.sndplay)(); }
void soundmng_stop(void)  { (*snddrv.sndstop)(); }

BRESULT soundmng_initialize(void)
{
	snddrv_setup();
	soundmng_pcminit();
	return SUCCESS;
}

void soundmng_deinitialize(void)
{
	soundmng_pcmdestroy();
	soundmng_destroy();
	buffer_destroy();
}

void soundmng_sync(void)
{
	struct sndbuf *sndbuf;
	const SINT32  *pcm;

	if (opened) {
		/* fill every free block: with short blocks a single block per
		 * call can be less than one device I/O period */
		sounddrv_lock();
		while ((sndbuf = SNDBUF_FREELIST_FIRST()) != NULL) {
			SNDBUF_FREELIST_REMOVE_HEAD();
			sounddrv_unlock();
			pcm = sound_pcmlock();
			if (pcm) {
				(*fnmix)((SINT16 *)sndbuf->buf, pcm, opna_frame);
			}
			sound_pcmunlock(pcm);
			sndbuf->remain = sndbuf->size;
			sounddrv_lock();
			SNDBUF_FILLED_QUEUE_INSERT_TAIL(sndbuf);
			if (!pcm) break;
		}
		sounddrv_unlock();
	}
}

void soundmng_setreverse(BOOL reverse)
{
	fnmix = reverse ? satuation_s16x : satuation_s16;
}

#if defined(__APPLE__)
static void coreaudio_setvolume(float gain);
#endif

void soundmng_setvolume(int volume)
{
	float gain = (float)volume / 100.0f;
#if defined(__APPLE__)
	if (np2oscfg.snddrv == SNDDRV_COREAUDIO) {
		coreaudio_setvolume(gain);
		return;
	}
#endif
	if (audio_st) {
		SDL_SetAudioStreamGain(audio_st, gain);
	}
}

/* ---- PCM ---- */
static void soundmng_pcminit(void)
{
	int i;
	for (i = 0; i < SOUND_MAXPCM; i++) pcm_channel[i] = NULL;
}

static void soundmng_pcmdestroy(void)
{
	pcm_channel_t *chan;
	int i;
	for (i = 0; i < SOUND_MAXPCM; i++) {
		chan = pcm_channel[i];
		if (chan) {
			pcm_channel[i] = NULL;
			if (chan->cookie) { (*snddrv.pcmdestroy)(chan->cookie, i); chan->cookie = NULL; }
			if (chan->path)   { _MFREE(chan->path); chan->path = NULL; }
		}
	}
}

void soundmng_pcmvolume(UINT num, int volume)
{
	pcm_channel_t *chan;
	if (num < SOUND_MAXPCM) {
		chan = pcm_channel[num];
		if (chan) {
			chan->volume = volume;
			if (chan->cookie) (*snddrv.pcmvolume)(chan->cookie, num, volume);
		}
	}
}

BRESULT soundmng_pcmplay(UINT num, BOOL loop)
{
	pcm_channel_t *chan;
	if (num < SOUND_MAXPCM) {
		chan = pcm_channel[num];
		if (chan && chan->cookie) (*snddrv.pcmplay)(chan->cookie, num, loop);
		return SUCCESS;
	}
	return FAILURE;
}

void soundmng_pcmstop(UINT num)
{
	pcm_channel_t *chan;
	if (num < SOUND_MAXPCM) {
		chan = pcm_channel[num];
		if (chan && chan->cookie) (*snddrv.pcmstop)(chan->cookie, num);
	}
}

/* ---- buffer management ---- */
static BRESULT buffer_init(void)
{
	int i;
	sounddrv_lock();
	for (i = 0; i < NSOUNDBUFFER; i++) {
		if (sound_buffer[i].buf) { _MFREE(sound_buffer[i].buf); sound_buffer[i].buf = NULL; }
		sound_buffer[i].buf = (char *)_MALLOC(opna_frame, "sound buffer");
		if (!sound_buffer[i].buf) {
			while (--i >= 0) { _MFREE(sound_buffer[i].buf); sound_buffer[i].buf = NULL; }
			sounddrv_unlock();
			return FAILURE;
		}
	}
	buffer_clear();
	sounddrv_unlock();
	return SUCCESS;
}

static void buffer_clear(void)
{
	int i;
	SNDBUF_FREELIST_INIT();
	SNDBUF_FILLED_QUEUE_INIT();
	for (i = 0; i < NSOUNDBUFFER; i++) {
		sound_buffer[i].next = &sound_buffer[i + 1];
		if (sound_buffer[i].buf) memset(sound_buffer[i].buf, 0, opna_frame);
		sound_buffer[i].size = sound_buffer[i].remain = opna_frame;
	}
	sound_buffer[NSOUNDBUFFER - 1].next = NULL;
	sndbuf_freelist = sound_buffer;
}

static void buffer_destroy(void)
{
	int i;
	sounddrv_lock();
	SNDBUF_FREELIST_INIT();
	SNDBUF_FILLED_QUEUE_INIT();
	for (i = 0; i < NSOUNDBUFFER; i++) {
		sound_buffer[i].next = NULL;
		if (sound_buffer[i].buf) { _MFREE(sound_buffer[i].buf); sound_buffer[i].buf = NULL; }
	}
	sounddrv_unlock();
}

/* ---- nosound driver ---- */
static BRESULT nosound_drvinit(UINT rate, UINT samples) { (void)rate; (void)samples; return SUCCESS; }
static BRESULT nosound_drvterm(void)                    { return SUCCESS; }
static void    nosound_drvlock(void)                    {}
static void    nosound_drvunlock(void)                  {}
static void    nosound_sndplay(void)                    {}
static void    nosound_sndstop(void)                    {}
static void   *nosound_pcmload(UINT num, const char *path) { (void)num; (void)path; return NULL; }
static void    nosound_pcmdestroy(void *cookie, UINT num)  { (void)cookie; (void)num; }
static void    nosound_pcmplay(void *cookie, UINT num, BOOL loop) { (void)cookie; (void)num; (void)loop; }
static void    nosound_pcmstop(void *cookie, UINT num)     { (void)cookie; (void)num; }
static void    nosound_pcmvolume(void *cookie, UINT num, int volume) { (void)cookie; (void)num; (void)volume; }

static BRESULT nosound_setup(void)
{
	snddrv.drvinit   = nosound_drvinit;
	snddrv.drvterm   = nosound_drvterm;
	snddrv.drvlock   = nosound_drvlock;
	snddrv.drvunlock = nosound_drvunlock;
	snddrv.sndplay   = nosound_sndplay;
	snddrv.sndstop   = nosound_sndstop;
	snddrv.pcmload   = nosound_pcmload;
	snddrv.pcmdestroy= nosound_pcmdestroy;
	snddrv.pcmplay   = nosound_pcmplay;
	snddrv.pcmstop   = nosound_pcmstop;
	snddrv.pcmvolume = nosound_pcmvolume;
	return SUCCESS;
}

/* ---- SDL3 audio driver ---- */
static void sdlaudio_callback(void *userdata, SDL_AudioStream *stream,
                               int additional_amount, int total_amount)
{
	struct sndbuf *sndbuf;
	(void)userdata;
	(void)total_amount;

	sndbuf_lock();
	while (additional_amount > 0) {
		sndbuf = SNDBUF_FILLED_QUEUE_FIRST();
		if (!sndbuf) break;
		int put = (int)MIN((UINT)additional_amount, sndbuf->remain);
		SDL_PutAudioStreamData(stream,
			sndbuf->buf + (sndbuf->size - sndbuf->remain), put);
		sndbuf->remain -= put;
		additional_amount -= put;
		if (sndbuf->remain == 0) {
			SNDBUF_FILLED_QUEUE_REMOVE_HEAD();
			SNDBUF_FREELIST_INSERT_HEAD(sndbuf);
		}
	}
	sndbuf_unlock();
}

static BRESULT sdlaudio_init(UINT rate, UINT samples)
{
	SDL_AudioSpec fmt;

	if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		g_printerr("sdlaudio_init: SDL_InitSubSystem: %s\n", SDL_GetError());
		return FAILURE;
	}
	fmt.freq     = (int)rate;
	fmt.format   = SDL_AUDIO_S16;
	fmt.channels = 2;

	audio_st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
	                                     &fmt, sdlaudio_callback, NULL);
	if (!audio_st) {
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return FAILURE;
	}
	SDL_ResumeAudioStreamDevice(audio_st);
	return SUCCESS;
}

static BRESULT sdlaudio_term(void)
{
	SDL_PauseAudioStreamDevice(audio_st);
	SDL_DestroyAudioStream(audio_st);
	return SUCCESS;
}

static void sdlaudio_lock(void)   { SDL_LockAudioStream(audio_st); }
static void sdlaudio_unlock(void) { SDL_UnlockAudioStream(audio_st); }
static void sdlaudio_play(void)   { SDL_ResumeAudioStreamDevice(audio_st); }
static void sdlaudio_stop(void)   { SDL_PauseAudioStreamDevice(audio_st); }

static BRESULT sdlaudio_setup(void)
{
	snddrv.drvinit   = sdlaudio_init;
	snddrv.drvterm   = sdlaudio_term;
	snddrv.drvlock   = sdlaudio_lock;
	snddrv.drvunlock = sdlaudio_unlock;
	snddrv.sndplay   = sdlaudio_play;
	snddrv.sndstop   = sdlaudio_stop;
	snddrv.pcmload   = nosound_pcmload;
	snddrv.pcmdestroy= nosound_pcmdestroy;
	snddrv.pcmplay   = nosound_pcmplay;
	snddrv.pcmstop   = nosound_pcmstop;
	snddrv.pcmvolume = nosound_pcmvolume;
	return SUCCESS;
}

/* ---- CoreAudio driver (macOS) ----
 * AUHAL default output with a small device I/O buffer; the render
 * callback pulls straight from the block queue. */
#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <os/lock.h>

/* device I/O buffer: ~2.7ms at 48kHz */
#define COREAUDIO_IOFRAMES  128

static AudioComponentInstance ca_unit = NULL;
static os_unfair_lock ca_lock = OS_UNFAIR_LOCK_INIT;
static float ca_gain = 1.0f;

static OSStatus coreaudio_render(void *refcon, AudioUnitRenderActionFlags *flags,
                                 const AudioTimeStamp *ts, UInt32 bus,
                                 UInt32 nframes, AudioBufferList *io)
{
	struct sndbuf *sndbuf;
	char *dst = (char *)io->mBuffers[0].mData;
	UINT  want = io->mBuffers[0].mDataByteSize;
	(void)refcon; (void)ts; (void)bus; (void)nframes;

	os_unfair_lock_lock(&ca_lock);
	while (want > 0) {
		sndbuf = SNDBUF_FILLED_QUEUE_FIRST();
		if (!sndbuf) break;
		UINT put = MIN(want, sndbuf->remain);
		memcpy(dst, sndbuf->buf + (sndbuf->size - sndbuf->remain), put);
		sndbuf->remain -= put;
		dst  += put;
		want -= put;
		if (sndbuf->remain == 0) {
			SNDBUF_FILLED_QUEUE_REMOVE_HEAD();
			SNDBUF_FREELIST_INSERT_HEAD(sndbuf);
		}
	}
	os_unfair_lock_unlock(&ca_lock);

	if (want > 0) {
		/* underrun: pad with silence */
		memset(dst, 0, want);
		if (want == io->mBuffers[0].mDataByteSize)
			*flags |= kAudioUnitRenderAction_OutputIsSilence;
	}
	return noErr;
}

static BRESULT coreaudio_init(UINT rate, UINT samples)
{
	AudioComponentDescription desc;
	AudioStreamBasicDescription fmt;
	AURenderCallbackStruct cb;
	AudioComponent comp;
	UInt32 ioframes;
	(void)samples;

	memset(&desc, 0, sizeof(desc));
	desc.componentType         = kAudioUnitType_Output;
	desc.componentSubType      = kAudioUnitSubType_DefaultOutput;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	comp = AudioComponentFindNext(NULL, &desc);
	if (!comp || AudioComponentInstanceNew(comp, &ca_unit) != noErr) {
		ca_unit = NULL;
		return FAILURE;
	}

	/* interleaved S16 stereo; the AU converts to the device format */
	memset(&fmt, 0, sizeof(fmt));
	fmt.mSampleRate       = (Float64)rate;
	fmt.mFormatID         = kAudioFormatLinearPCM;
	fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
	fmt.mChannelsPerFrame = 2;
	fmt.mBitsPerChannel   = 16;
	fmt.mFramesPerPacket  = 1;
	fmt.mBytesPerFrame    = 4;
	fmt.mBytesPerPacket   = 4;
	if (AudioUnitSetProperty(ca_unit, kAudioUnitProperty_StreamFormat,
	                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt)) != noErr) {
		goto err;
	}

	/* Request a small HAL buffer. Best effort: the device may clamp it,
	 * and Bluetooth outputs add their own latency regardless. */
	ioframes = COREAUDIO_IOFRAMES * (rate > 48000 ? 2 : 1);
	AudioUnitSetProperty(ca_unit, kAudioDevicePropertyBufferFrameSize,
	                     kAudioUnitScope_Global, 0, &ioframes, sizeof(ioframes));

	cb.inputProc       = coreaudio_render;
	cb.inputProcRefCon = NULL;
	if (AudioUnitSetProperty(ca_unit, kAudioUnitProperty_SetRenderCallback,
	                         kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr) {
		goto err;
	}
	if (AudioUnitInitialize(ca_unit) != noErr) {
		goto err;
	}
	coreaudio_setvolume(ca_gain);
	return SUCCESS;

err:
	AudioComponentInstanceDispose(ca_unit);
	ca_unit = NULL;
	return FAILURE;
}

static BRESULT coreaudio_term(void)
{
	if (ca_unit) {
		AudioOutputUnitStop(ca_unit);
		AudioUnitUninitialize(ca_unit);
		AudioComponentInstanceDispose(ca_unit);
		ca_unit = NULL;
	}
	return SUCCESS;
}

static void coreaudio_lock(void)   { os_unfair_lock_lock(&ca_lock); }
static void coreaudio_unlock(void) { os_unfair_lock_unlock(&ca_lock); }
static void coreaudio_play(void)   { if (ca_unit) AudioOutputUnitStart(ca_unit); }
static void coreaudio_stop(void)   { if (ca_unit) AudioOutputUnitStop(ca_unit); }

static void coreaudio_setvolume(float gain)
{
	ca_gain = gain;
	if (ca_unit) {
		AudioUnitSetParameter(ca_unit, kHALOutputParam_Volume,
		                      kAudioUnitScope_Global, 0, gain, 0);
	}
}

static BRESULT coreaudio_setup(void)
{
	snddrv.drvinit   = coreaudio_init;
	snddrv.drvterm   = coreaudio_term;
	snddrv.drvlock   = coreaudio_lock;
	snddrv.drvunlock = coreaudio_unlock;
	snddrv.sndplay   = coreaudio_play;
	snddrv.sndstop   = coreaudio_stop;
	snddrv.pcmload   = nosound_pcmload;
	snddrv.pcmdestroy= nosound_pcmdestroy;
	snddrv.pcmplay   = nosound_pcmplay;
	snddrv.pcmstop   = nosound_pcmstop;
	snddrv.pcmvolume = nosound_pcmvolume;
	return SUCCESS;
}

#endif /* __APPLE__ */

BRESULT soundmng_pcmload(UINT num, const char *filename)
{
	(void)num; (void)filename;
	return FAILURE;
}

#endif /* !NOSOUND */
