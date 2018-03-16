
#include <stdio.h>
#include <stdlib.h>

#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>

#include "libavutil/enc_connection.h"
#include "libavutil/log.h"

typedef struct EncMsg {
    char* msg;
    uint16_t type;
} EncMsg;

typedef struct MsgList {
    struct EncMsg* msg;
    struct MsgList* next;
} MsgList;

typedef struct MsgQueue {
    MsgList *first, *last;
    int nb_msgs;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} MsgQueue;

static int msg_queue_init(MsgQueue* q)
{
    int ret = 0;
    memset(q, 0, sizeof(MsgQueue));
    if ((ret = pthread_mutex_init(&q->mutex, NULL)))
        return AVERROR(ret);
    if ((ret = pthread_cond_init(&q->cond, NULL)))
        return AVERROR(ret);
    return 0;
}

static void msg_queue_end(MsgQueue *q)
{
    MsgList *msg, *msg1;

    pthread_mutex_lock(&q->mutex);
    for (msg = q->first; msg != NULL; msg = msg1) {
        msg1 = msg->next;
        free(msg->msg->msg);
        free(msg->msg);
        free(msg);
    }
    q->last   = NULL;
    q->first  = NULL;
    q->nb_msgs = 0;

    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static int msg_queue_put(MsgQueue* q, EncMsg* msg, int64_t queue_size)
{
    MsgList* msg_entry;

    int ret = 0;
    pthread_mutex_lock(&q->mutex);

    if (queue_size > 0 && q->nb_msgs >= queue_size) {
        ret = AVERROR(ENOBUFS);

        msg_entry = q->first;

        if (msg_entry) {
            q->first = msg_entry->next;

            if (!q->first) {
                q->last = NULL;
            }
            q->nb_msgs--;
            free(msg_entry->msg->msg);
            free(msg_entry->msg);
            free(msg_entry);
        }
    }

    msg_entry = (MsgList*)malloc(sizeof(MsgList));
    if (!msg_entry) {
        pthread_mutex_unlock(&q->mutex);
        return AVERROR(ENOMEM);
    }

    msg_entry->next = NULL;
    msg_entry->msg = msg;

    if (!q->last) {
        q->first = msg_entry;
    } else {
        q->last->next = msg_entry;
    }

    q->last = msg_entry;
    q->nb_msgs++;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

static int msg_queue_get(MsgQueue* q, EncMsg* msg, int block)
{
    MsgList *msg_entry;
    int ret = 0;

    pthread_mutex_lock(&q->mutex);

    for (;;) {
        msg_entry = q->first;
        if (msg_entry) {
            q->first = msg_entry->next;
            if (!q->first) {
                q->last = NULL;
            }
            q->nb_msgs--;
            *msg = *(msg_entry->msg);
            free(msg_entry->msg);
            free(msg_entry);
            break;
        } else if (!block) {
            ret = AVERROR(EAGAIN);
            break;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

// how to get info?
char* enc_connection = NULL;

static int sockfd = -1;
static pthread_t thread;
static int stop = 0;

static MsgQueue queue;

static void* connect_thread(void* arg)
{
    EncMsg* msg = malloc(sizeof(EncMsg));
    char buf[256];

    while (!stop) {
        // connect to encoder tools

        msg_queue_get(&queue, msg, 1);
        if (msg->type == ENC_MSG_LAST) {
            free(msg->msg);
            break;
        }

        if (sockfd > 0 && read(sockfd, buf, sizeof(buf)-1) == 0) {
            av_log(NULL, AV_LOG_WARNING, "ENC Disconnected\n");
            close(sockfd);
            sockfd = -1;
        }

        while (sockfd <= 0) {
            int portno;
            struct sockaddr_in serv_addr;
            struct hostent *server;
            struct timeval tv;

            char ipString[20];

            char* port = strchr(enc_connection, ':');
            if (port != NULL) {
                port++;
            }

            portno = atoi(port);

            /* Create a socket point */
            sockfd = socket(AF_INET, SOCK_STREAM, 0);

            tv.tv_sec = 0;
            tv.tv_usec = 100;
            setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

            if (sockfd < 0) {
                av_log(NULL, AV_LOG_ERROR, "ERROR opening socket\n");
                usleep(100000);
                continue;
            }

            strncpy(ipString, enc_connection, port - enc_connection - 1);

            printf("ENC Connection => %s : %s\n", ipString, port);

            server = gethostbyname(ipString);

            if (server == NULL) {
                av_log(NULL, AV_LOG_ERROR, "ERROR, no such host\n");
                exit(0);
            }

            memset(&serv_addr, 0, sizeof(serv_addr));
            serv_addr.sin_family = AF_INET;
            memcpy((char *)&serv_addr.sin_addr.s_addr, (char *)server->h_addr_list[0], server->h_length);
            serv_addr.sin_port = htons(portno);

            /* Now connect to the server */
            if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
                av_log(NULL, AV_LOG_ERROR, "ERROR connecting ENC\n");
                close(sockfd);
                sockfd = -1;
                usleep(1000000);
                continue;
            }

            av_log(NULL, AV_LOG_WARNING, "ENC Connected\n");
            break;
        }

        {
            int suc = 1;
            uint16_t n = ntohs(2 + strlen(msg->msg));
            uint16_t swappedType = ntohs(msg->type);

            suc = suc && (write(sockfd, &n, 2) >= 0);
            suc = suc && (write(sockfd, &swappedType, 2) >= 0);
            suc = suc && (write(sockfd, msg->msg, strlen(msg->msg)) >= 0);

            if (n < 0) {
                av_log(NULL, AV_LOG_ERROR, "ERROR writing to socket\n");
                sockfd = -1;
            }

            free(msg->msg);
        }
    }

    free(msg);

    return NULL;
}

void enc_connection_init()
{
    int ret;

    if (enc_connection != NULL) {
        if ((ret = pthread_create(&thread, NULL, connect_thread, NULL))) {
            av_log(NULL, AV_LOG_ERROR, "Can not create enc connection thread\n");
        }

        msg_queue_init(&queue);
    }
}

void enc_connection_stop()
{
    if (enc_connection != NULL) {
        stop = 1;
        enc_send(ENC_MSG_LAST, "");
        pthread_join(thread, NULL);
        msg_queue_end(&queue);
    }
}

void enc_send(int type, const char* message)
{
    if (enc_connection != NULL) {
        EncMsg* msg = malloc(sizeof(EncMsg));
        if (msg) {
            msg->msg = malloc(strlen(message) + 1);
            memcpy(msg->msg, message, strlen(message) + 1);
            msg->type = type;
            msg_queue_put(&queue, msg, type);
        }
    }
}
