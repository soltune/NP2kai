/* === NP2kai wx port - debug monitor (TCP control port, NP2KAI_MONITOR=<port>) === */

#ifndef NP2_WX_DBGMON_H
#define NP2_WX_DBGMON_H

void dbgmon_initialize(void);
/* run queued commands on the emulator thread; true while paused */
bool dbgmon_poll(void);

#endif
