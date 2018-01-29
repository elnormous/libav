
#ifndef enc_connection_h
#define enc_connection_h

#include <stdio.h>

void enc_connection_init(void);
void enc_connection_stop(void);
void enc_send(int type, const char* message);

#define ENC_MSG_LOG             1
#define ENC_MSG_BLACK_FRAME     2
#define ENC_MSG_AUDIO_LVL       3

#endif /* enc_connection_h */
