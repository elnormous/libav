
#ifndef evo_connection_h
#define evo_connection_h

#include <stdio.h>

void evo_connection_init(void);
void evo_connection_stop(void);
void evo_send(int type, const char* message);

#define EVO_MSG_LOG             1
#define EVO_MSG_BLACK_FRAME     2
#define EVO_MSG_AUDIO_LVL       3

#endif /* evo_connection_h */
