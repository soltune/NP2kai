/* === NP2kai wx port - debug monitor (TCP control port) ===
 *
 * Started when NP2KAI_MONITOR=<port> is set. Listens on 127.0.0.1:<port>;
 * one client at a time sends text commands, one per line ("help" lists them).
 * Commands run on the emulator thread between frames (dbgmon_poll), so they
 * see a consistent machine state. Collectors (hot/trace/io/atapi) arm a hook
 * in the CPU, I/O or ATAPI core and reply once they have gathered enough.
 */

#include <compiler.h>
#include "dbgmon.h"

#if defined(CPUCORE_IA32)

#include "mousemng.h"

#include <cpucore.h>
#include <pccore.h>
#include <keystat.h>
#include <io/iocore.h>
#include <cbus/ideio.h>
#include <cbus/atapicmd.h>
#include <fdd/sxsi.h>
#include <vram/scrnsave.h>
#include <sound/fmboard.h>

#include <string>
#include <vector>
#include <deque>
#include <map>
#include <algorithm>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- connection ---- */

static std::mutex              s_lock;
static std::deque<std::string> s_cmds;
static int                     s_client = -1;
static int                     s_listen = -1;
static std::atomic<bool>       s_active(false);
static bool                    s_paused = false;

static void reply(const std::string &text)
{
	std::lock_guard<std::mutex> g(s_lock);
	if (s_client < 0) {
		return;
	}
	std::string out = text;
	if (out.empty() || out[out.size() - 1] != '\n') {
		out += '\n';
	}
	out += ".\n";	/* end of reply */
	const char *p = out.c_str();
	size_t left = out.size();
	while (left) {
		ssize_t n = send(s_client, p, left, 0);
		if (n <= 0) {
			break;
		}
		p += n;
		left -= (size_t)n;
	}
}

static std::string fmt(const char *f, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}

static void listen_thread(void)
{
	for (;;) {
		int c = accept(s_listen, NULL, NULL);
		if (c < 0) {
			return;
		}
		{
			std::lock_guard<std::mutex> g(s_lock);
			s_client = c;
		}
		std::string line;
		char buf[512];
		ssize_t n;
		while ((n = recv(c, buf, sizeof(buf), 0)) > 0) {
			for (ssize_t i = 0; i < n; i++) {
				if (buf[i] == '\n') {
					if (!line.empty() && line[line.size() - 1] == '\r') {
						line.erase(line.size() - 1);
					}
					std::lock_guard<std::mutex> g(s_lock);
					s_cmds.push_back(line);
					line.clear();
				}
				else {
					line += buf[i];
				}
			}
		}
		std::lock_guard<std::mutex> g(s_lock);
		close(c);
		s_client = -1;
	}
}

/* ---- memory access (no exceptions: walks the page tables by hand) ---- */

static UINT32 lin2phys(UINT32 laddr)
{
	if (CPU_STAT_PAGING) {
		return convert_laddr_to_paddr(laddr);
	}
	return laddr;
}

static UINT8 read_lin(UINT32 laddr)
{
	return (UINT8)memp_read8(lin2phys(laddr));
}

/* ADDR: p:HEX physical, l:HEX or HEX linear, cs:/ds:/es:/ss:/fs:/gs:OFF via
 * the segment base, SSSS:OFF as a real-mode segment */
static bool parse_addr(const std::string &s, UINT32 *laddr, bool *phys)
{
	static const char *segs[] = {"es", "cs", "ss", "ds", "fs", "gs"};
	*phys = false;
	size_t colon = s.find(':');
	if (colon == std::string::npos) {
		*laddr = (UINT32)strtoul(s.c_str(), NULL, 16);
		return true;
	}
	std::string a = s.substr(0, colon);
	UINT32 off = (UINT32)strtoul(s.c_str() + colon + 1, NULL, 16);
	if (a == "p") {
		*phys = true;
		*laddr = off;
		return true;
	}
	if (a == "l") {
		*laddr = off;
		return true;
	}
	for (int i = 0; i < 6; i++) {
		if (a == segs[i]) {
			*laddr = CPU_STAT_SREGBASE(i) + off;
			return true;
		}
	}
	*laddr = ((UINT32)strtoul(a.c_str(), NULL, 16) << 4) + off;
	return true;
}

static std::string hexdump(UINT32 addr, bool phys, UINT len)
{
	std::string out;
	for (UINT i = 0; i < len; i += 16) {
		std::string hex, asc;
		for (UINT j = 0; j < 16 && i + j < len; j++) {
			UINT32 a = addr + i + j;
			UINT8 b = phys ? (UINT8)memp_read8(a) : read_lin(a);
			hex += fmt("%02x ", b);
			asc += (b >= 0x20 && b < 0x7f) ? (char)b : '.';
		}
		out += fmt("%08x: %-48s %s\n", addr + i, hex.c_str(), asc.c_str());
	}
	return out;
}

/* ---- machine state ---- */

static const char *cpumode(void)
{
	if (!CPU_STAT_PM) {
		return "real";
	}
	if (CPU_STAT_VM86) {
		return "v86";
	}
	return "protected";
}

static std::string regs_text(void)
{
	std::string s;
	s += fmt("%s mode, CPL %d, %s code, paging %s\n", cpumode(), CPU_STAT_CPL,
	         CPU_STATSAVE.cpu_inst_default.op_32 ? "32-bit" : "16-bit",
	         CPU_STAT_PAGING ? "on" : "off");
	s += fmt("cs:eip %04x:%08x (linear %08x)\n", CPU_CS, CPU_EIP,
	         CPU_STAT_SREGBASE(CPU_CS_INDEX) + CPU_EIP);
	s += fmt("eax %08x ebx %08x ecx %08x edx %08x\n", CPU_EAX, CPU_EBX, CPU_ECX, CPU_EDX);
	s += fmt("esi %08x edi %08x ebp %08x esp %08x\n", CPU_ESI, CPU_EDI, CPU_EBP, CPU_ESP);
	s += fmt("ds %04x es %04x ss %04x fs %04x gs %04x eflags %08x (IF=%d)\n",
	         CPU_DS, CPU_ES, CPU_SS, CPU_FS, CPU_GS, CPU_EFLAG, (CPU_EFLAG & I_FLAG) ? 1 : 0);
	s += fmt("cr0 %08x cr3 %08x\n", CPU_CR0, CPU_CR3);
	s += fmt("pic master imr %02x irr %02x isr %02x / slave imr %02x irr %02x isr %02x\n",
	         pic.pi[0].imr, pic.pi[0].irr, pic.pi[0].isr,
	         pic.pi[1].imr, pic.pi[1].irr, pic.pi[1].isr);
	s += fmt("next: %s\n", cpu_disasm2str(CPU_EIP));
	return s;
}

static std::string cd_text(void)
{
	std::string s;
	s += fmt("daplaying bits %02x\n", ideio.daplaying);
	for (int d = 0; d < 2; d++) {
		for (int u = 0; u < 2; u++) {
			IDEDRV drv = &ideio.dev[d].drv[u];
			if (drv->device != IDETYPE_CDROM) {
				continue;
			}
			UINT f = drv->daflag;
			const char *st = (f == 0x11) ? "playing" : (f == 0x12) ? "paused" :
			                 (f == 0x13) ? "completed" : (f == 0x14) ? "error" :
			                 (f == 0x15) ? "no status" : "?";
			s += fmt("ide%d.%d: audio status %02x (%s), current lba %u, remaining %u sectors, msf %s\n",
			         d, u, f, st, drv->dacurpos, drv->dalength, drv->damsfbcd ? "bcd" : "binary");
			s += fmt("  status %02x error %02x sense %02x asc %04x\n",
			         drv->status, drv->error, drv->sk, drv->asc);
		}
	}
	return s;
}

/* sound board interrupt sources: OPNA timer flags, 86-PCM FIFO interrupt,
 * the line they share and the PIC */
static std::string snd_text(void)
{
	std::string s;
	for (UINT i = 0; i < NELEMENTS(g_opna); i++) {
		const OPNA &o = g_opna[i];
		if (o.s.irq == 0xff) {
			continue;
		}
		std::string run;
		if (i < 2) {
			run = fmt(", timer A %s, timer B %s",
			          nevent_iswork(i ? NEVENT_FMTIMER2A : NEVENT_FMTIMERA) ? "running" : "stopped",
			          nevent_iswork(i ? NEVENT_FMTIMER2B : NEVENT_FMTIMERB) ? "running" : "stopped");
		}
		s += fmt("opna%u: irq %u, status %02x, reg 24-27 %02x %02x %02x %02x%s\n", i, o.s.irq,
		         o.s.status, o.s.reg[0x24], o.s.reg[0x25], o.s.reg[0x26], o.s.reg[0x27], run.c_str());
	}
	if (g_pcm86.irq != 0xff) {
		s += fmt("pcm86: irq %u, fifo %02x, irqflag %u, irqline %u, irqseen %u, irqfill %u, reqirq %u, virbuf %d, fifosize %d, event %s\n",
		         g_pcm86.irq, g_pcm86.fifo, g_pcm86.irqflag, g_pcm86.irqline, g_pcm86.irqseen, g_pcm86.irqfill,
		         g_pcm86.reqirq, g_pcm86.virbuf, g_pcm86.fifosize, nevent_iswork(NEVENT_86PCM) ? "scheduled" : "none");
	}
	for (UINT irq = 0; irq < 16; irq++) {
		bool used = (g_pcm86.irq == irq);
		for (UINT i = 0; i < NELEMENTS(g_opna); i++) {
			used = used || (g_opna[i].s.irq == irq);
		}
		if (used) {
			const _PICITEM &p = pic.pi[irq >> 3];
			UINT bit = 1 << (irq & 7);
			s += fmt("irq %u: line %s, pic imr %d irr %d isr %d\n", irq,
			         fmboard_irqline((REG8)irq) ? "up" : "down",
			         (p.imr & bit) ? 1 : 0, (p.irr & bit) ? 1 : 0, (p.isr & bit) ? 1 : 0);
		}
	}
	return s;
}

/* ---- collectors ---- */

/* hot: CS:EIP histogram over the next N instructions */
static UINT32 s_hotLeft = 0;
static UINT32 s_hotTotal = 0;
static std::map<UINT64, UINT32> s_hot;
static std::map<UINT64, UINT32> s_hotBase;	/* key -> linear address */

/* trace: the next N instructions, disassembled */
static UINT32 s_traceLeft = 0;
static std::vector<std::string> s_trace;

/* history: a ring of the last N instructions with their code bytes and
 * registers, kept until a break range is entered, which freezes it and
 * pauses the machine (the bytes are disassembled offline) */
struct HistEntry {
	UINT32 lin, eip;
	UINT32 eax, ebx, ecx, edx, esi, edi, ebp, esp;
	UINT16 cs, ds, es, ss;
	UINT32 eflags;
	UINT8  mode;	/* 'r'eal, 'p'rotected, 'v'86; upper case = 32-bit code */
	UINT8  code[11];
};
static std::vector<HistEntry> s_hist;
static size_t s_histPos = 0;
static UINT64 s_histCount = 0;
static bool s_histOn = false;
/* break conditions: any of them pauses the machine */
struct BreakCond {
	UINT32 lo, hi;		/* linear range of CS:EIP */
	int    sreg;		/* segment register index that must hold sregval, or -1 */
	UINT16 sregval;
	int    iflag;		/* IF must be 0 or 1, or -1 */
	int    nzaddr;		/* the byte at this linear address must be non-zero, or -1 */
	std::string text;
};
static std::vector<BreakCond> s_brk;
static bool s_brkArmed = false, s_brkHit = false;
static size_t s_brkWhich = 0;
static std::string s_brkNote;	/* set when a break has paused the machine */

static void hist_record(void)
{
	HistEntry &e = s_hist[s_histPos];
	e.cs = CPU_CS;
	e.eip = CPU_EIP;
	e.lin = CPU_STAT_SREGBASE(CPU_CS_INDEX) + CPU_EIP;
	e.eax = CPU_EAX; e.ebx = CPU_EBX; e.ecx = CPU_ECX; e.edx = CPU_EDX;
	e.esi = CPU_ESI; e.edi = CPU_EDI; e.ebp = CPU_EBP; e.esp = CPU_ESP;
	e.ds = CPU_DS; e.es = CPU_ES; e.ss = CPU_SS;
	e.eflags = CPU_EFLAG;
	char m = !CPU_STAT_PM ? 'r' : CPU_STAT_VM86 ? 'v' : 'p';
	e.mode = CPU_STATSAVE.cpu_inst_default.op_32 ? (m - 0x20) : m;
	UINT32 p = lin2phys(e.lin);
	for (UINT i = 0; i < sizeof(e.code); i++) {
		if (i && ((e.lin + i) & 0xfff) == 0) {
			p = lin2phys(e.lin + i) - i;
		}
		e.code[i] = (UINT8)memp_read8(p + i);
	}
	s_histPos = (s_histPos + 1) % s_hist.size();
	s_histCount++;
}

static std::string hist_text(UINT n)
{
	size_t size = s_hist.size();
	size_t have = (size_t)std::min<UINT64>(s_histCount, size);
	n = (UINT)std::min<size_t>(n, have);
	std::string s;
	for (size_t i = 0; i < n; i++) {
		const HistEntry &e = s_hist[(s_histPos + size - n + i) % size];
		std::string code;
		for (UINT j = 0; j < sizeof(e.code); j++) {
			code += fmt("%02x", e.code[j]);
		}
		s += fmt("%c %04x:%08x %08x %s | eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x "
		         "ebp %08x esp %08x ds %04x es %04x ss %04x fl %08x\n",
		         e.mode, e.cs, e.eip, e.lin, code.c_str(), e.eax, e.ebx, e.ecx, e.edx,
		         e.esi, e.edi, e.ebp, e.esp, e.ds, e.es, e.ss, e.eflags);
	}
	return s;
}

/* histio: the history entries that access a port range (the value read
 * shows in the next entry's eax) or execute INT n, oldest first */
static std::string histio_text(UINT lo, UINT hi, UINT n)
{
	size_t size = s_hist.size();
	size_t have = (size_t)std::min<UINT64>(s_histCount, size);
	n = (UINT)std::min<size_t>(n, have);
	std::string s;
	for (size_t i = 0; i < n; i++) {
		const HistEntry &e = s_hist[(s_histPos + size - n + i) % size];
		UINT k = 0;
		while (k < 4 && (e.code[k] == 0x66 || e.code[k] == 0x67 || e.code[k] == 0x2e ||
		                 e.code[k] == 0x26 || e.code[k] == 0x36 || e.code[k] == 0x3e ||
		                 e.code[k] == 0x64 || e.code[k] == 0x65 || e.code[k] == 0xf2 || e.code[k] == 0xf3)) {
			k++;
		}
		UINT8 op = e.code[k];
		std::string what;
		if (op >= 0xec && op <= 0xef) {
			UINT port = e.edx & 0xffff;
			if (port >= lo && port <= hi) {
				what = fmt("%s %04x", (op & 2) ? "out" : "in ", port);
			}
		}
		else if (op >= 0xe4 && op <= 0xe7) {
			UINT port = e.code[k + 1];
			if (port >= lo && port <= hi) {
				what = fmt("%s %04x", (op & 2) ? "out" : "in ", port);
			}
		}
		else if (op == 0xcd) {
			what = fmt("int %02x", e.code[k + 1]);
		}
		if (what.empty()) {
			continue;
		}
		UINT32 nexteax = (i + 1 < n) ? s_hist[(s_histPos + size - n + i + 1) % size].eax : 0;
		s += fmt("%7u %c %04x:%08x %-8s eax %08x (next %08x) ebx %08x ecx %08x edx %08x\n",
		         (UINT)i, e.mode, e.cs, e.eip, what.c_str(), e.eax, nexteax, e.ebx, e.ecx, e.edx);
	}
	return s;
}

static void stephook(void)
{
	if (s_histOn) {
		hist_record();
	}
	if (s_brkArmed) {
		UINT32 lin = CPU_STAT_SREGBASE(CPU_CS_INDEX) + CPU_EIP;
		for (size_t i = 0; i < s_brk.size(); i++) {
			const BreakCond &b = s_brk[i];
			if (lin < b.lo || lin > b.hi ||
			    (b.sreg >= 0 && CPU_REGS_SREG(b.sreg) != b.sregval) ||
			    (b.iflag >= 0 && ((CPU_EFLAG & I_FLAG) ? 1 : 0) != b.iflag) ||
			    (b.nzaddr >= 0 && read_lin((UINT32)b.nzaddr) == 0)) {
				continue;
			}
			s_brkArmed = false;
			s_brkHit = true;
			s_brkWhich = i;
			s_histOn = false;	/* freeze; dbgmon_poll pauses */
			break;
		}
	}
	if (s_hotLeft) {
		UINT64 key = ((UINT64)CPU_CS << 32) | CPU_EIP;
		s_hot[key]++;
		if (s_hotBase.find(key) == s_hotBase.end()) {
			s_hotBase[key] = CPU_STAT_SREGBASE(CPU_CS_INDEX) + CPU_EIP;
		}
		s_hotLeft--;
	}
	if (s_traceLeft) {
		s_trace.push_back(fmt("%s  ; eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x fl %04x",
		                      cpu_disasm2str(CPU_EIP), CPU_EAX, CPU_EBX, CPU_ECX, CPU_EDX,
		                      CPU_ESI, CPU_EDI, CPU_FLAG));
		s_traceLeft--;
	}
	if (!s_hotLeft && !s_traceLeft && !s_histOn && !s_brkArmed) {
		cpu_stephook = NULL;
	}
}

static std::string hot_text(void)
{
	std::vector<std::pair<UINT32, UINT64> > v;
	for (auto &e : s_hot) {
		v.push_back(std::make_pair(e.second, e.first));
	}
	std::sort(v.rbegin(), v.rend());
	std::string s = fmt("%u instructions, %u distinct addresses\n", s_hotTotal, (UINT)v.size());
	for (size_t i = 0; i < v.size() && i < 40; i++) {
		UINT32 lin = s_hotBase[v[i].second];
		std::string bytes;
		for (int j = 0; j < 8; j++) {
			bytes += fmt("%02x ", read_lin(lin + j));
		}
		s += fmt("%8u  %04x:%08x  lin %08x  %s\n", v[i].first,
		         (UINT)(v[i].second >> 32), (UINT32)v[i].second, lin, bytes.c_str());
	}
	return s;
}

/* io: port accesses in a range */
static UINT s_ioLo = 0, s_ioHi = 0;
static UINT32 s_ioLeft = 0;
static std::vector<std::string> s_io;
static bool s_ioCollapse = true;

static void iohook(UINT port, UINT32 dat, int size, int dir)
{
	if (port < s_ioLo || port > s_ioHi || !s_ioLeft) {
		return;
	}
	std::string line = fmt("%04x:%08x %s%s %04x %0*x", CPU_CS, CPU_PREV_EIP,
	                       dir ? "out" : "in ", size == 1 ? "b" : size == 2 ? "w" : "d",
	                       port, size * 2, dat);
	/* polling loops repeat the same access; fold them into a count */
	if (s_ioCollapse && !s_io.empty()) {
		std::string &last = s_io.back();
		if (last.compare(0, line.size(), line) == 0 &&
		    (last.size() == line.size() || last[line.size()] == ' ')) {
			UINT rep = 1;
			size_t x = last.find(" x", line.size());
			if (x != std::string::npos) {
				rep = (UINT)strtoul(last.c_str() + x + 2, NULL, 10);
			}
			last = line + fmt(" x%u", rep + 1);
			return;
		}
	}
	s_io.push_back(line);
	s_ioLeft--;
}

/* atapi: packet commands */
static UINT32 s_atapiLeft = 0;
static std::vector<std::string> s_atapi;

static void atapihook(IDEDRV drv, const UINT8 *cdb)
{
	if (!s_atapiLeft) {
		return;
	}
	std::string s = fmt("%04x:%08x cdb", CPU_CS, CPU_PREV_EIP);
	for (int i = 0; i < 12; i++) {
		s += fmt(" %02x", cdb[i]);
	}
	s += fmt(" | status %02x err %02x", drv->status, drv->error);
	if (drv->bufsize) {
		s += " reply";
		for (UINT i = 0; i < drv->bufsize && i < 16; i++) {
			s += fmt(" %02x", drv->buf[i]);
		}
	}
	s += fmt(" | audio %02x lba %u left %u", drv->daflag, drv->dacurpos, drv->dalength);
	/* the same poll over and over: count it instead of listing it */
	if (!s_atapi.empty()) {
		std::string &last = s_atapi.back();
		size_t bar = s.find(" | audio");
		if (last.compare(0, bar, s, 0, bar) == 0) {
			UINT rep = 1;
			size_t x = last.rfind(" x");
			if (x != std::string::npos && x > last.find(" | audio")) {
				rep = (UINT)strtoul(last.c_str() + x + 2, NULL, 10);
			}
			last = s + fmt(" x%u", rep + 1);
			return;
		}
	}
	s_atapi.push_back(s);
	s_atapiLeft--;
}

/* irq: interrupt requests (pic_setirq) and deliveries per IRQ over a span
 * of emulated time */
static UINT32 s_irqReq[16], s_irqDel[16];
static UINT32 s_irqStart = 0, s_irqLen = 0;
static bool s_irqWait = false;

/* the CPU clock count wraps (32 bits); differences of it stay right */
static UINT32 emuclock(void)
{
	return (UINT32)(CPU_CLOCK + CPU_BASECLOCK - CPU_REMCLOCK);
}

/* irqlog: the last requests and deliveries, with the function that called
 * pic_setirq and whether the IRQ was already pending or in service */
struct IrqLogEntry {
	UINT32 clock;
	UINT32 eip;
	UINT16 cs;
	UINT8  irq, delivered, irr, isr;
	void   *caller;
};
static IrqLogEntry s_irqLog[64];
static UINT s_irqLogPos = 0;
static UINT64 s_irqLogCount = 0;
static bool s_irqLogOn = false;
static int s_irqLogOnly = -1;	/* log this IRQ only (-1 = all) */
/* requests made while the IRQ is in service: they become pending and are
 * taken as soon as the handler sends EOI and enables interrupts, which is
 * how a driver that is not reentrant gets entered twice */
static IrqLogEntry s_irqBusy[16];
static UINT s_irqBusyPos = 0;
static UINT64 s_irqBusyCount = 0;

static void pichook(REG8 irq, int delivered)
{
	if (s_irqWait && irq < 16) {
		(delivered ? s_irqDel : s_irqReq)[irq]++;
	}
	if (s_irqLogOn && (s_irqLogOnly < 0 || s_irqLogOnly == irq)) {
		IrqLogEntry &e = s_irqLog[s_irqLogPos];
		const _PICITEM &p = pic.pi[(irq >> 3) & 1];
		UINT bit = 1 << (irq & 7);
		e.clock = emuclock();
		e.cs = CPU_CS;
		e.eip = CPU_EIP;
		e.irq = irq;
		e.delivered = (UINT8)delivered;
		e.irr = (p.irr & bit) ? 1 : 0;
		e.isr = (p.isr & bit) ? 1 : 0;
		/* frame 0 returns into pic_setirq / pic_irq, frame 1 into its caller
		 * (macOS keeps frame pointers, so walking one frame up is safe) */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wframe-address"
		e.caller = __builtin_return_address(1);
#pragma clang diagnostic pop
		s_irqLogPos = (s_irqLogPos + 1) % NELEMENTS(s_irqLog);
		s_irqLogCount++;
		if (!delivered && e.isr) {
			s_irqBusy[s_irqBusyPos] = e;
			s_irqBusyPos = (s_irqBusyPos + 1) % NELEMENTS(s_irqBusy);
			s_irqBusyCount++;
		}
	}
}

static std::string irqlog_lines(const IrqLogEntry *ring, UINT size, UINT pos, UINT n)
{
	std::string s;
	UINT32 prev = 0;
	for (UINT i = 0; i < n; i++) {
		const IrqLogEntry &e = ring[(pos + size - n + i) % size];
		std::string who = "?";
		Dl_info info;
		if (e.caller && dladdr(e.caller, &info) && info.dli_sname) {
			who = fmt("%s+0x%lx", info.dli_sname,
			          (unsigned long)((const char *)e.caller - (const char *)info.dli_saddr));
		}
		double us = i ? (UINT32)(e.clock - prev) * 1e6 / pccore.realclock : 0;
		prev = e.clock;
		s += fmt("%10.1f  irq %2u %-9s %-28s at %04x:%08x  irr %u isr %u\n", us, e.irq,
		         e.delivered ? "delivered" : "request", e.delivered ? "" : who.c_str(),
		         e.cs, e.eip, e.irr, e.isr);
	}
	return s;
}

static std::string irqlog_text(UINT n)
{
	UINT size = NELEMENTS(s_irqLog);
	n = (UINT)std::min<UINT64>(std::min(n, size), s_irqLogCount);
	std::string s = fmt("%llu logged, last %u (time in us from the previous line; irr/isr: "
	                    "before a request, after a delivery)\n", (unsigned long long)s_irqLogCount, n);
	s += irqlog_lines(s_irqLog, size, s_irqLogPos, n);
	UINT busy = (UINT)std::min<UINT64>(s_irqBusyCount, NELEMENTS(s_irqBusy));
	s += fmt("%llu requests while the IRQ was in service, last %u:\n",
	         (unsigned long long)s_irqBusyCount, busy);
	s += irqlog_lines(s_irqBusy, NELEMENTS(s_irqBusy), s_irqBusyPos, busy);
	return s;
}

static std::string irq_text(void)
{
	double sec = (double)s_irqLen / pccore.realclock;
	std::string s = fmt("%.2f s of emulated time\n", sec);
	for (int i = 0; i < 16; i++) {
		if (s_irqReq[i] || s_irqDel[i]) {
			s += fmt("irq %2d: requested %7u (%8.1f/s), delivered %7u (%8.1f/s)\n", i,
			         s_irqReq[i], s_irqReq[i] / sec, s_irqDel[i], s_irqDel[i] / sec);
		}
	}
	return s;
}

/* key: held keys and when to release them (games that poll the key state
 * miss a press and release sent in the same instant) */
static std::vector<std::pair<REG8, std::chrono::steady_clock::time_point> > s_keyRelease;
/* click: mouse buttons held the same way (0 = left, 1 = right) */
static std::vector<std::pair<int, std::chrono::steady_clock::time_point> > s_btnRelease;

static void release_keys(bool all)
{
	auto now = std::chrono::steady_clock::now();
	for (size_t i = 0; i < s_keyRelease.size(); ) {
		if (all || now >= s_keyRelease[i].second) {
			keystat_keyup(s_keyRelease[i].first);
			s_keyRelease.erase(s_keyRelease.begin() + i);
		}
		else {
			i++;
		}
	}
	for (size_t i = 0; i < s_btnRelease.size(); ) {
		if (all || now >= s_btnRelease[i].second) {
			if (s_btnRelease[i].first) {
				mousemng_onright(FALSE);
			}
			else {
				mousemng_onleft(FALSE);
			}
			s_btnRelease.erase(s_btnRelease.begin() + i);
		}
		else {
			i++;
		}
	}
}

static std::string join(const std::vector<std::string> &v)
{
	std::string s;
	for (auto &l : v) {
		s += l;
		s += '\n';
	}
	return s;
}

/* deferred replies are sent from dbgmon_poll once the collector is done */
static bool s_hotWait = false, s_traceWait = false, s_ioWait = false, s_atapiWait = false;

static void finish_collectors(void)
{
	if (s_hotWait && !s_hotLeft) {
		s_hotWait = false;
		reply(hot_text());
	}
	if (s_traceWait && !s_traceLeft) {
		s_traceWait = false;
		reply(join(s_trace));
	}
	if (s_ioWait && !s_ioLeft) {
		s_ioWait = false;
		iocore_hook = NULL;
		reply(join(s_io));
	}
	if (s_atapiWait && !s_atapiLeft) {
		s_atapiWait = false;
		atapi_cmdhook = NULL;
		reply(join(s_atapi));
	}
	if (s_irqWait && (UINT32)(emuclock() - s_irqStart) >= s_irqLen) {
		s_irqWait = false;
		if (!s_irqLogOn) {
			pic_hook = NULL;
		}
		reply(irq_text());
	}
	if (s_brkHit) {
		/* no reply here: a client waiting for it would keep the port
		 * busy; 'status' reports the break instead */
		s_brkHit = false;
		s_paused = true;
		s_brkNote = "break: " + s_brk[s_brkWhich].text + ", 'hist N' shows the instructions before it";
	}
}

/* ---- commands ---- */

static const char *s_help =
	"status | regs            CPU registers, mode, PIC, next instruction\n"
	"mem ADDR [LEN]           hex dump (ADDR: p:HEX phys, l:HEX linear, ds:OFF, SSSS:OFF real-mode)\n"
	"dis [OFF] [N]            disassemble N instructions at CS:OFF (default CS:EIP)\n"
	"hot [N]                  CS:EIP histogram over the next N instructions (default 2000000)\n"
	"trace [N]                disassemble the next N instructions as they run (default 200)\n"
	"io LO[-HI] [N] [all]     log the next N port accesses in the range (repeats folded unless 'all')\n"
	"atapi [N]                log the next N ATAPI packet commands with their replies\n"
	"irq [SEC]                count interrupt requests and deliveries per IRQ over SEC seconds\n"
	"                         of emulated time (default 1)\n"
	"irqlog on [IRQ] | off    keep the last 64 requests/deliveries (with the caller of pic_setirq)\n"
	"irqlog [N]               print the last N of them\n"
	"history [N]              keep the last N instructions (code bytes, registers); 0 = off\n"
	"break LO[-HI] [ss=XXXX] [if=0|1] [nz=LINEAR]\n"
	"                         add a condition: CS:EIP in the linear range (with that segment\n"
	"                         register value / interrupt flag / a non-zero byte at that linear\n"
	"                         address, e.g. DOS's InDOS flag); the first one met freezes\n"
	"                         history and pauses ('status' shows it). 'break clear' removes\n"
	"                         them. Start 'history' first to keep the instructions before it\n"
	"hist [N]                 print the last N history entries (mode cs:eip linear bytes | regs)\n"
	"histio LO[-HI] [N]       the history entries (of the last N) that access those ports or\n"
	"                         execute INT n, oldest first, numbered from the oldest\n"
	"stop                     end the collectors now and send what they have\n"
	"cd                       CD-DA state of the IDE CD-ROM drives\n"
	"snd                      sound board interrupt sources (OPNA timers, 86-PCM) and the PIC\n"
	"pause | continue         stop or resume emulation\n"
	"screenshot FILE.bmp      save the emulated screen as it is drawn now\n"
	"key HEX [HEX...] [/MS]   press PC-98 key codes together, release after MS (default 100)\n"
	"move DX DY               move the mouse by DX, DY\n"
	"click [l|r] [/MS]        press a mouse button, release after MS (default 100)\n"
	"help\n";

static std::vector<std::string> split(const std::string &s)
{
	std::vector<std::string> v;
	size_t i = 0;
	while (i < s.size()) {
		while (i < s.size() && s[i] == ' ') i++;
		size_t j = i;
		while (j < s.size() && s[j] != ' ') j++;
		if (j > i) v.push_back(s.substr(i, j - i));
		i = j;
	}
	return v;
}

static void run_command(const std::string &line)
{
	std::vector<std::string> a = split(line);
	if (a.empty()) {
		return;
	}
	const std::string &c = a[0];
	if (c == "help") {
		reply(s_help);
	}
	else if (c == "status" || c == "regs") {
		std::string st = !s_paused ? "running" : s_brkNote.empty() ? "paused" : "paused (" + s_brkNote + ")";
		reply(st + "\n" + regs_text());
	}
	else if (c == "mem" && a.size() >= 2) {
		UINT32 addr;
		bool phys;
		parse_addr(a[1], &addr, &phys);
		UINT len = (a.size() >= 3) ? (UINT)strtoul(a[2].c_str(), NULL, 16) : 0x80;
		reply(hexdump(addr, phys, std::min(len, 0x10000u)));
	}
	else if (c == "dis") {
		UINT32 eip = (a.size() >= 2) ? (UINT32)strtoul(a[1].c_str(), NULL, 16) : CPU_EIP;
		UINT n = (a.size() >= 3) ? (UINT)strtoul(a[2].c_str(), NULL, 10) : 16;
		std::string s;
		for (UINT i = 0; i < n && i < 1000; i++) {
			disasm_context_t d;
			UINT32 next = eip;
			if (disasm(&next, &d) != 0) {
				s += fmt("%04x:%08x: (cannot decode)\n", CPU_CS, eip);
				break;
			}
			s += cpu_disasm2str(eip);
			s += '\n';
			eip = next;
		}
		reply(s);
	}
	else if (c == "hot") {
		s_hotTotal = s_hotLeft = (a.size() >= 2) ? (UINT32)strtoul(a[1].c_str(), NULL, 10) : 2000000;
		s_hot.clear();
		s_hotBase.clear();
		s_hotWait = true;
		cpu_stephook = stephook;
	}
	else if (c == "trace") {
		s_traceLeft = (a.size() >= 2) ? (UINT32)strtoul(a[1].c_str(), NULL, 10) : 200;
		s_traceLeft = std::min(s_traceLeft, (UINT32)100000);
		s_trace.clear();
		s_traceWait = true;
		cpu_stephook = stephook;
	}
	else if (c == "io" && a.size() >= 2) {
		size_t dash = a[1].find('-');
		s_ioLo = (UINT)strtoul(a[1].c_str(), NULL, 16);
		s_ioHi = (dash != std::string::npos) ? (UINT)strtoul(a[1].c_str() + dash + 1, NULL, 16) : s_ioLo;
		s_ioLeft = (a.size() >= 3) ? (UINT32)strtoul(a[2].c_str(), NULL, 10) : 200;
		s_ioCollapse = !(a.size() >= 4 && a[3] == "all");
		s_io.clear();
		s_ioWait = true;
		iocore_hook = iohook;
	}
	else if (c == "atapi") {
		s_atapiLeft = (a.size() >= 2) ? (UINT32)strtoul(a[1].c_str(), NULL, 10) : 50;
		s_atapi.clear();
		s_atapiWait = true;
		atapi_cmdhook = atapihook;
	}
	else if (c == "irq") {
		double sec = (a.size() >= 2) ? strtod(a[1].c_str(), NULL) : 1.0;
		memset(s_irqReq, 0, sizeof(s_irqReq));
		memset(s_irqDel, 0, sizeof(s_irqDel));
		s_irqLen = (UINT32)(std::min(std::max(sec, 0.01) * pccore.realclock, 4.0e9));
		s_irqStart = emuclock();
		s_irqWait = true;
		pic_hook = pichook;
	}
	else if (c == "irqlog") {
		if (a.size() >= 2 && a[1] == "on") {
			s_irqLogOnly = (a.size() >= 3) ? atoi(a[2].c_str()) : -1;
			s_irqLogPos = 0;
			s_irqLogCount = 0;
			s_irqBusyPos = 0;
			s_irqBusyCount = 0;
			s_irqLogOn = true;
			pic_hook = pichook;
			reply("logging");
		}
		else if (a.size() >= 2 && a[1] == "off") {
			s_irqLogOn = false;
			if (!s_irqWait) {
				pic_hook = NULL;
			}
			reply("off");
		}
		else {
			reply(irqlog_text((a.size() >= 2) ? (UINT)strtoul(a[1].c_str(), NULL, 10) : 32));
		}
	}
	else if (c == "history") {
		UINT n = (a.size() >= 2) ? (UINT)strtoul(a[1].c_str(), NULL, 10) : 4000;
		s_histOn = false;
		s_hist.assign(std::min(n, 4000000u), HistEntry());
		s_histPos = 0;
		s_histCount = 0;
		if (!s_hist.empty()) {
			s_histOn = true;
			cpu_stephook = stephook;
		}
		reply(s_histOn ? fmt("recording the last %u instructions", n) : std::string("history off"));
	}
	else if (c == "break" && a.size() >= 2 && a[1] == "clear") {
		s_brk.clear();
		s_brkArmed = false;
		reply("no break conditions");
	}
	else if (c == "break" && a.size() >= 2) {
		/* break LO[-HI] [ss=XXXX] [if=0|1]: adds a condition (the linear
		 * address range of CS:EIP, optionally a segment register value and
		 * the interrupt flag); any of them pauses. Without 'history' first
		 * only the conditions are checked, which costs little speed */
		static const char *segs[] = {"es", "cs", "ss", "ds", "fs", "gs"};
		BreakCond b;
		size_t dash = a[1].find('-');
		b.lo = (UINT32)strtoul(a[1].c_str(), NULL, 16);
		b.hi = (dash != std::string::npos) ? (UINT32)strtoul(a[1].c_str() + dash + 1, NULL, 16) : b.lo;
		b.sreg = -1;
		b.sregval = 0;
		b.iflag = -1;
		b.nzaddr = -1;
		b.text = fmt("%08x-%08x", b.lo, b.hi);
		for (size_t j = 2; j < a.size(); j++) {
			if (a[j].size() > 3 && a[j][2] == '=') {
				if (a[j].compare(0, 2, "if") == 0) {
					b.iflag = atoi(a[j].c_str() + 3) ? 1 : 0;
				}
				if (a[j].compare(0, 2, "nz") == 0) {
					b.nzaddr = (int)strtoul(a[j].c_str() + 3, NULL, 16);
				}
				for (int i = 0; i < 6; i++) {
					if (a[j].compare(0, 2, segs[i]) == 0) {
						b.sreg = i;
						b.sregval = (UINT16)strtoul(a[j].c_str() + 3, NULL, 16);
					}
				}
				b.text += " " + a[j];
			}
		}
		s_brk.push_back(b);
		s_brkArmed = true;
		cpu_stephook = stephook;
		reply(fmt("armed (%u conditions): %s; 'status' shows when it has paused",
		          (UINT)s_brk.size(), b.text.c_str()));
	}
	else if (c == "histio" && a.size() >= 2) {
		size_t dash = a[1].find('-');
		UINT lo = (UINT)strtoul(a[1].c_str(), NULL, 16);
		UINT hi = (dash != std::string::npos) ? (UINT)strtoul(a[1].c_str() + dash + 1, NULL, 16) : lo;
		UINT n = (a.size() >= 3) ? (UINT)strtoul(a[2].c_str(), NULL, 10) : 0xffffffffu;
		reply(histio_text(lo, hi, n));
	}
	else if (c == "hist") {
		UINT n = (a.size() >= 2) ? (UINT)strtoul(a[1].c_str(), NULL, 10) : 200;
		reply(fmt("%llu recorded%s\n", (unsigned long long)s_histCount, s_histOn ? " (still recording)" : "") +
		      hist_text(n));
	}
	else if (c == "stop") {
		bool armed = s_brkArmed || s_histOn;
		s_hotLeft = s_traceLeft = s_ioLeft = s_atapiLeft = 0;
		s_histOn = s_brkArmed = false;
		s_brk.clear();
		cpu_stephook = NULL;
		if (s_irqWait) {
			s_irqLen = std::max((UINT32)(emuclock() - s_irqStart), (UINT32)1);	/* counts so far */
		}
		if (!s_hotWait && !s_traceWait && !s_ioWait && !s_atapiWait && !s_irqWait) {
			reply(armed ? "history and break stopped" : "nothing running");
		}
	}
	else if (c == "cd") {
		reply(cd_text());
	}
	else if (c == "snd") {
		reply(snd_text());
	}
	else if (c == "pause") {
		s_paused = true;
		reply(regs_text());
	}
	else if (c == "continue" || c == "c") {
		s_paused = false;
		s_brkNote.clear();
		reply("running");
	}
	else if (c == "screenshot" && a.size() >= 2) {
		SCRNSAVE ss = scrnsave_create();
		BRESULT r = ss ? scrnsave_writebmp(ss, a[1].c_str(), SCRNSAVE_AUTO) : FAILURE;
		if (ss) {
			scrnsave_destroy(ss);
		}
		reply(r == SUCCESS ? "saved" : "failed");
	}
	else if (c == "key" && a.size() >= 2) {
		/* keys are pressed together and held; a trailing "/MS" sets
		 * the hold time (default 100 ms) */
		UINT ms = 100;
		size_t n = a.size();
		if (a[n - 1][0] == '/') {
			ms = (UINT)strtoul(a[n - 1].c_str() + 1, NULL, 10);
			n--;
		}
		release_keys(true);
		auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
		for (size_t i = 1; i < n; i++) {
			REG8 k = (REG8)strtoul(a[i].c_str(), NULL, 16);
			keystat_keydown(k);
			s_keyRelease.push_back(std::make_pair(k, until));
		}
		reply("ok");
	}
	else if (c == "move" && a.size() >= 3) {
		mousemng_onmove(atoi(a[1].c_str()), atoi(a[2].c_str()));
		reply("ok");
	}
	else if (c == "click") {
		/* click [l|r] [/MS]: hold the button for MS (default 100) */
		int right = (a.size() >= 2 && a[1] == "r") ? 1 : 0;
		UINT ms = 100;
		if (a.size() >= 2 && a[a.size() - 1][0] == '/') {
			ms = (UINT)strtoul(a[a.size() - 1].c_str() + 1, NULL, 10);
		}
		release_keys(true);
		if (right) {
			mousemng_onright(TRUE);
		}
		else {
			mousemng_onleft(TRUE);
		}
		s_btnRelease.push_back(std::make_pair(right,
		                       std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)));
		reply("ok");
	}
	else {
		reply("unknown command (try help)");
	}
}

/* ---- public ---- */

void dbgmon_initialize(void)
{
	const char *env = getenv("NP2KAI_MONITOR");
	if (!env || !*env) {
		return;
	}
	int port = atoi(env);
	s_listen = socket(AF_INET, SOCK_STREAM, 0);
	if (s_listen < 0) {
		return;
	}
	int on = 1;
	setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s_listen, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(s_listen, 1) < 0) {
		fprintf(stderr, "dbgmon: cannot listen on 127.0.0.1:%d\n", port);
		close(s_listen);
		s_listen = -1;
		return;
	}
	fprintf(stderr, "dbgmon: listening on 127.0.0.1:%d\n", port);
	s_active = true;
	std::thread(listen_thread).detach();
}

bool dbgmon_poll(void)
{
	if (!s_active) {
		return false;
	}
	for (;;) {
		std::string cmd;
		{
			std::lock_guard<std::mutex> g(s_lock);
			if (s_cmds.empty()) {
				break;
			}
			cmd = s_cmds.front();
			s_cmds.pop_front();
		}
		run_command(cmd);
	}
	release_keys(false);
	finish_collectors();
	return s_paused;
}

#else	/* !CPUCORE_IA32 */

void dbgmon_initialize(void) {}
bool dbgmon_poll(void) { return false; }

#endif
