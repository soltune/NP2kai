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

#include <cpucore.h>
#include <pccore.h>
#include <keystat.h>
#include <io/iocore.h>
#include <cbus/ideio.h>
#include <cbus/atapicmd.h>
#include <fdd/sxsi.h>
#include <vram/scrnsave.h>

#include <string>
#include <vector>
#include <deque>
#include <map>
#include <algorithm>
#include <mutex>
#include <thread>
#include <atomic>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
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

/* ---- collectors ---- */

/* hot: CS:EIP histogram over the next N instructions */
static UINT32 s_hotLeft = 0;
static UINT32 s_hotTotal = 0;
static std::map<UINT64, UINT32> s_hot;
static std::map<UINT64, UINT32> s_hotBase;	/* key -> linear address */

/* trace: the next N instructions, disassembled */
static UINT32 s_traceLeft = 0;
static std::vector<std::string> s_trace;

static void stephook(void)
{
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
	if (!s_hotLeft && !s_traceLeft) {
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
	"stop                     end the collectors now and send what they have\n"
	"cd                       CD-DA state of the IDE CD-ROM drives\n"
	"pause | continue         stop or resume emulation\n"
	"screenshot FILE.bmp      save the emulated screen as it is drawn now\n"
	"key HEX [HEX...]         press and release PC-98 key codes (e.g. key 1c = RETURN)\n"
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
		reply(std::string(s_paused ? "paused\n" : "running\n") + regs_text());
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
	else if (c == "stop") {
		s_hotLeft = s_traceLeft = s_ioLeft = s_atapiLeft = 0;
		cpu_stephook = NULL;
		if (!s_hotWait && !s_traceWait && !s_ioWait && !s_atapiWait) {
			reply("nothing running");
		}
	}
	else if (c == "cd") {
		reply(cd_text());
	}
	else if (c == "pause") {
		s_paused = true;
		reply(regs_text());
	}
	else if (c == "continue" || c == "c") {
		s_paused = false;
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
		for (size_t i = 1; i < a.size(); i++) {
			REG8 k = (REG8)strtoul(a[i].c_str(), NULL, 16);
			keystat_keydown(k);
			keystat_keyup(k);
		}
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
	finish_collectors();
	return s_paused;
}

#else	/* !CPUCORE_IA32 */

void dbgmon_initialize(void) {}
bool dbgmon_poll(void) { return false; }

#endif
