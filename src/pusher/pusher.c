/*
 * Copyright (c) 2013-2015 Anton Dobkin <anton.dobkin@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>

#include "apn.h"
#include "apn_array.h"
#include "apn_payload.h"
#include "apn_strings.h"
#include "apn_strerror.h"

static void __apn_logging(apn_log_levels level, const char *const message, uint32_t len) {
    (void )len;
    const char *prefix = NULL;
    switch (level) {
        case APN_LOG_LEVEL_INFO:
            prefix = "inf";
            break;
        case APN_LOG_LEVEL_ERROR:
            prefix = "err";
            break;
        case APN_LOG_LEVEL_DEBUG:
            prefix = "dbg";
            break;
    }
    fprintf(stdout, "======> [apn][%s]: %s\n", prefix, message);
}

static void __apn_token_free(void *data) {
    free(data);
}

static apn_array_t *__apn_split_tokens(char *const tokens) {
    apn_array_t *array = apn_array_init(20, __apn_token_free, NULL);
    if (array) {
        char *p = strtok(tokens, ":");
        while (p) {
            char *token = apn_strndup(p, strlen(p));
            apn_array_insert(array, (void *) token);
            p = strtok(NULL, ":");
        }
        return array;
    }
    return NULL;
}

apn_array_t *__apn_read_tokens_from_file(const char *const path) {
    int fd = open(path, O_RDONLY);
    if(fd < 0) {
        return NULL;
    }
    struct stat fs;
    if (-1 == fstat(fd, &fs)) {
        close(fd);
        return NULL;
    }
    char *buffer = mmap(0, fs.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if ((void*) -1 == buffer) {
        close(fd);
        return NULL;
    }

    char *buffer_end = buffer + fs.st_size;
    char *p_start = buffer;
    char *p_end = buffer;

    apn_array_t *tokens = apn_array_init(20, __apn_token_free, NULL);
    if(!tokens) {
        close(fd);
        return NULL;
    }

    for(;;) {
        if(*p_end != '\n') {
            if (++p_end < buffer_end) {
                continue;
            }
        }
        size_t token_size = (p_end - p_start) + 1;
        char *token = malloc(token_size);
        if(!token) {
            apn_array_free(tokens);
            return NULL;
        }
        memset(token, 0, token_size);
        apn_substr(token, p_start, token_size, 0, token_size - 1);
        apn_array_insert(tokens, token);
        if ((p_start = ++p_end) >= buffer_end) {
            break;
        }
    }
    munmap(buffer, fs.st_size);
    close(fd);
    return tokens;
}

static void __apn_pusher_usage(void) {
    fprintf(stderr, "apn-pusher - simple tool to send push notifications to iOS and OS X devices\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage: apn-pusher [OPTION]\n");
    fprintf(stderr, "    -h Print this message and exit\n");
    fprintf(stderr, "    -c Path to certificate file (required)\n");
    fprintf(stderr, "    -d Use sandbox mode\n");
    fprintf(stderr, "    -m Body of the alert to send in notification\n");
    fprintf(stderr, "    -e KEY:STRING Send custom message as key string value pair. value=STR\n");
    fprintf(stderr, "    -n KEY:STRING Send custom message as key string value pair. value=INT\n");
    fprintf(stderr, "    -a Indicates content available\n");
    fprintf(stderr, "    -b Badge number to set with notification\n");
    fprintf(stderr, "    -s Name of a sound file in the app bundle\n");
    fprintf(stderr, "    -i Name of an image file in the app bundle\n");
    fprintf(stderr, "    -y Category name of notification\n");
    fprintf(stderr, "    -t Tokens, separated with ':' (required)\n");
    fprintf(stderr, "    -T Path to file with tokens\n");
    fprintf(stderr, "    -v Make the operation more talkative\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        __apn_pusher_usage();
        return 1;
    }

    assert(apn_library_init() == APN_SUCCESS);
    setvbuf(stderr, NULL, _IOLBF, 0);

    apn_ctx_t *apn_ctx = apn_init();
    if (NULL == apn_ctx) {
        fprintf(stderr, "Unable to init context: %d\n", errno);
        apn_library_free();
        return -1;
    }

    apn_payload_t *payload = apn_payload_init();
    if (NULL == payload) {
        fprintf(stderr, "Unable to init payload: %d\n", errno);
        apn_free(apn_ctx);
        apn_library_free();
        return -1;
    }

    apn_payload_set_priority(payload, APN_NOTIFICATION_PRIORITY_HIGH);
    apn_set_behavior(apn_ctx, APN_OPTION_RECONNECT);

    apn_array_t *tokens = NULL;
    char *cert = NULL;
    uint8_t ret = 0;

    const char *const opts = "ahc:pdm:b:s:i:e:n:y:t:T:v";
    int c = -1;

    /* For -e option */
    char* field = NULL;
    char* key = NULL;  /* The key of custom message. */
    char* value = NULL;
    int len = 0;
    int field_cnt = 0;

    while ((c = getopt(argc, argv, opts)) != -1) {
        switch (c) {
            case 'h':
                __apn_pusher_usage();
                return 1;
            case 'd':
                apn_set_mode(apn_ctx, APN_MODE_SANDBOX);
                break;
            case 'b':
                apn_payload_set_badge(payload, atoi(optarg));
                break;
            case 'm':
                apn_payload_set_body(payload, optarg);
                break;
            case 'n':
                field = strtok(optarg, ":");
                if(field != NULL)
                {
                    field_cnt += 1;
                    len = strlen(field);
                    key = malloc(len+1);
                    memset(key, 0, len+1);
                    strncpy(key, field, len);
                }
                
                while(1) 
                {
                   field = strtok(NULL, ":");
                   if(field != NULL)
                   {
                       field_cnt += 1;
                   } else {
                       break;
                   }
                   
                   if(field_cnt == 2)
                   {
                       len = strlen(field);
                       value = malloc(len+1);
                       memset(value, 0, len+1);
                       strncpy(value, field, len);
                   }
                }
                if(field_cnt != 2)
                {
                    fprintf(stderr, "Wrong parameter for -e\n");
                    free(key);
                    free(value);
                    goto finish;
                }
                apn_payload_add_custom_property_integer(payload, key, value);
                free(key);
                free(value);
                key = NULL;
                value = NULL;
                field_cnt = 0;
                break;
            case 'e':
                field = strtok(optarg, ":");
                if(field != NULL)
                {
                    field_cnt += 1;
                    len = strlen(field);
                    key = malloc(len+1);
                    memset(key, 0, len+1);
                    strncpy(key, field, len);
                }
                
                while(1) 
                {
                   field = strtok(NULL, ":");
                   if(field != NULL)
                   {
                       field_cnt += 1;
                   } else {
                       break;
                   }
                   
                   if(field_cnt == 2)
                   {
                       len = strlen(field);
                       value = malloc(len+1);
                       memset(value, 0, len+1);
                       strncpy(value, field, len);
                   }
                }
                if(field_cnt != 2)
                {
                    fprintf(stderr, "Wrong parameter for -e\n");
                    free(key);
                    free(value);
                    goto finish;
                }
                apn_payload_add_custom_property_string(payload, key, value);
                free(key);
                free(value);
                key = NULL;
                value = NULL;
                field_cnt = 0;
                break;
            case 'c':
                cert = apn_strndup(optarg, strlen(optarg));
                break;
            case 's':
                apn_payload_set_sound(payload, optarg);
                break;
            case 'i':
                apn_payload_set_launch_image(payload, optarg);
                break;
            case 'y':
                apn_payload_set_category(payload, optarg);
                break;
            case 't':
                tokens = __apn_split_tokens(optarg);
                break;
            case 'T':
                tokens = __apn_read_tokens_from_file(optarg);
                if(!tokens) {
                    char error[250] = {0};
                    apn_strerror(errno, error, sizeof(error) - 1);
                    fprintf(stderr, "Unable to parse file %s: %s (errno: %d).\n", optarg, error, errno);
                    goto finish;
                }
                break;
            case 'a':
                apn_payload_set_content_available(payload, 1);
                break;
            case 'v':
                apn_set_log_callback(apn_ctx, __apn_logging);
                apn_set_log_level(apn_ctx, APN_LOG_LEVEL_INFO | APN_LOG_LEVEL_ERROR);
                break;
            case '?':
                if (optopt == 'c') {
                    fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                } else if (isprint(optopt)) {
                    fprintf(stderr, "Unknown option `-%c'.\n", optopt);
                } else {
                    fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
                }
                goto finish;
        }
    }

    if (cert) {
        apn_set_certificate(apn_ctx, cert, cert, NULL);
    } else {
        fprintf(stderr, "Missing cert file\n");
        ret = 1;
        goto finish;
    }

    if (!tokens || apn_array_count(tokens) == 0) {
        fprintf(stderr, "Missing device token\n");
        ret = 1;
        goto finish;
    }

    if (APN_ERROR == apn_connect(apn_ctx)) {
        char *error = apn_error_string(errno);
        fprintf(stderr, "Could not connected to Apple Push Notification Service: %s (errno: %d)\n", error, errno);
        ret = 1;
        free(error);
    } else {
        apn_array_t *invalid_tokens = NULL;
        if (APN_ERROR == apn_send(apn_ctx, payload, tokens, &invalid_tokens)) {
            ret = 1;
            char *error = apn_error_string(errno);
            fprintf(stderr, "Could not send push: %s (errno: %d)\n", error, errno);
            free(error);
        } else {
            fprintf(stderr, "Notification was sucessfully sent to %u device(s)\n",
                    apn_array_count(tokens) - ((invalid_tokens) ? apn_array_count(invalid_tokens) : 0));
        }

        if (invalid_tokens) {
            fprintf(stderr, "\n");
            fprintf(stderr, "Invalid tokens:\n");
            uint32_t i = 0;
            for (; i < apn_array_count(invalid_tokens); i++) {
                fprintf(stderr, "    %u. %s\n", i, (const char *)apn_array_item_at_index(invalid_tokens, i));
            }
            fprintf(stderr, "\n");
            apn_array_free(invalid_tokens);
        }
    }

    finish:
    apn_strfree(&cert);

    apn_free(apn_ctx);
    apn_payload_free(payload);
    apn_array_free(tokens);
    apn_library_free();

    return ret;
}
