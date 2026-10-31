/**
 * @file	pcm86.h
 * @brief	Interface of the 86-PCM
 */

#pragma once

#include <sound/sound.h>
#include <nevent.h>

enum {
	PCM86_LOGICALBUF	= 0x8000,
	PCM86_BUFSIZE		= (1 << 16),
	PCM86_BUFMSK		= ((1 << 16) - 1),

	PCM86_DIVBIT		= 10,
	PCM86_DIVENV		= (1 << PCM86_DIVBIT),

	PCM86_RESCUE		= 20
};

#define	PCM86_EXTBUF		g_pcm86.rescue					/* 救済延滞… */
#define	PCM86_REALBUFSIZE	(PCM86_LOGICALBUF + PCM86_EXTBUF)

void RECALC_NOWCLKWAIT(UINT64 cnt);
//#define RECALC_NOWCLKWAIT(cnt)											¥
//	do																	¥
//	{																	¥
//		SINT64 decvalue =(SINT64)(cnt << g_pcm86.stepbit);				¥
//		if (g_pcm86.virbuf - decvalue < g_pcm86.virbuf) 				¥
//		{																¥
//			g_pcm86.virbuf -= decvalue;									¥
//		}																¥
//		if (g_pcm86.virbuf < 0)											¥
//		{																¥
//			g_pcm86.virbuf &= g_pcm86.stepmask;							¥
//		}																¥
//	} while (0 /*CONSTCOND*/)

typedef struct {
	SINT32	divremain;
	SINT32	div;
	SINT32	div2;
	SINT32	smp;
	SINT32	lastsmp;
	SINT32	smp_l;
	SINT32	lastsmp_l;
	SINT32	smp_r;
	SINT32	lastsmp_r;

	UINT32	readpos;			/* DSOUND再生位置 */
	UINT32	wrtpos;				/* 書込み位置 */
	SINT32	realbuf;			/* DSOUND用のデータ数 */
	SINT32	virbuf;				/* 86PCM(bufsize:0x8000)のデータ数 */
	SINT32	rescue;

	SINT32	fifosize;
	SINT32	volume;
	SINT32	vol5;

	union {
		struct {
			UINT32	lastclock_obsolate;
			UINT32	stepclock_obsolate;
		} obsolate;
		UINT64	lastclockforwait;
	};
	UINT	stepmask;

	UINT8	fifo;
	UINT8	soundflags;			/*!< サウンド フラグ (A460) */
	UINT8	dactrl;
	UINT8	_write;
	UINT8	stepbit;
	UINT8	irq;
	UINT8	reqirq;
	UINT8	irqflag;

	UINT8	buffer[PCM86_BUFSIZE];
	
	UINT	rateval;
	
	UINT64	lastclock;
	UINT64	stepclock;

	UINT8	irqline;			/*!< FIFO割り込みで割り込み線を上げている (fmboard_irqline) */
	UINT8	irqseen;			/*!< ドライバが割り込みフラグを 1 で読んだ（クリアまで） */
	UINT8	irqfill;			/*!< その後にデータが書き込まれた（クリアまで） */
} _PCM86, *PCM86;

typedef struct { // ステートセーブ互換性維持用（変更禁止）
	SINT32	divremain;
	SINT32	div;
	SINT32	div2;
	SINT32	smp;
	SINT32	lastsmp;
	SINT32	smp_l;
	SINT32	lastsmp_l;
	SINT32	smp_r;
	SINT32	lastsmp_r;

	UINT32	readpos;			/* DSOUND再生位置 */
	UINT32	wrtpos;				/* 書込み位置 */
	SINT32	realbuf;			/* DSOUND用のデータ数 */
	SINT32	virbuf;				/* 86PCM(bufsize:0x8000)のデータ数 */
	SINT32	rescue;

	SINT32	fifosize;
	SINT32	volume;
	SINT32	vol5;

	UINT32	lastclock;
	UINT32	stepclock;
	UINT	stepmask;

	UINT8	fifo;
	UINT8	soundflags;			/*!< サウンド フラグ (A460) */
	UINT8	dactrl;
	UINT8	_write;
	UINT8	stepbit;
	UINT8	irq;
	UINT8	reqirq;
	UINT8	irqflag;

	UINT8	buffer[PCM86_BUFSIZE];
} _PCM86_OLD, *PCM86_OLD;

typedef struct {
	UINT	rate;
	UINT	vol;
} PCM86CFG;

/* デバッグ用の計数（デバッグモニタの snd で表示） */
typedef struct {
	UINT32	underrun;			/*!< 生成側のデータが尽きて出力を途中で打ち切った回数 */
	UINT32	overflow;			/*!< 生成側のバッファがあふれて捨てたバイト数 */
	UINT32	catchup;			/*!< pcm86gen_checkbuf が virbuf を生成側に合わせて減らした回数 */
	UINT32	slowdown;			/*!< pcm86gen_checkbuf が virbuf を増やした回数（生成側が大きく遅れたとき） */
} PCM86DBG;


#ifdef __cplusplus
extern "C"
{
#endif

extern const UINT pcm86rate8[];
extern PCM86DBG pcm86dbg;
/* デバッグモニタ用: 86PCM だけの出力（ステレオ、soundcfg.rate）。鳴っていない間は pcm = NULL */
extern void (*pcm86gen_hook)(const SINT32 *pcm, UINT count);

void pcm86_cb(NEVENTITEM item);

void pcm86cs_initialize();
void pcm86cs_shutdown();
void pcm86cs_enter_criticalsection();
void pcm86cs_leave_criticalsection();

void pcm86_reset(void);
void pcm86gen_initialize(UINT rate);
void pcm86gen_setvol(UINT vol);
void pcm86gen_update(void);
void pcm86_setpcmrate(REG8 val);
void pcm86_setnextintr(void);
void pcm86_changeclock(UINT oldmultiple);

void SOUNDCALL pcm86gen_checkbuf(PCM86 pcm86, UINT nCount);
void SOUNDCALL pcm86gen_getpcm(PCM86 pcm86, SINT32 *lpBuffer, UINT nCount);

BOOL pcm86gen_intrq(int fromFMTimer);

#ifdef __cplusplus
}
#endif
