#include "usyslog.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <stdbool.h>
#include <errno.h>

// Macros
#define RFC3164_MAX_BYTES       1024
#define RFC5424_MAX_BYTES       2048
#define MAX_BUFFER_SIZE         (RFC5424_MAX_BYTES * 32)

#ifndef ULLONG_MAX
#   define ULLONG_MAX ((uint64_t) -1)   // 2^64-1
#endif

#define IS_WS(c)            (c ==' ' || c == '\t' || c == '\r' || c == '\n')

#define LOWER(c)            (unsigned char)(c | 0x20)
#define IS_ALPHA(c)         (LOWER(c) >= 'a' && LOWER(c) <= 'z')
#define IS_NUM(c)           ((c) >= '0' && (c) <= '9')
#define IS_ALPHANUM(c)      (IS_ALPHA(c) || IS_NUM(c))

#define IS_HOST_CHAR(c) (IS_ALPHANUM(c) || (c) == '.' || (c) == '-' || (c) == '_')


enum token_state {
    ts_before,
    ts_read
};

// States
enum syslog_state {

    // Message Head
    s_msg_start,
    s_octet_count,
    s_priority_start,
    s_priority,
    s_version,
    s_timestamp,
    s_hostname,
    s_appname,
    s_procid,
    s_msgid,

    // RFC5424 - SDATA
    s_sd_start,
    s_sd_element,
    s_sd_field,
    s_sd_field_end,
    s_sd_value_begin,
    s_sd_value,
    s_sd_value_end,
    s_sd_end,

    // Message Content
    s_message,
    s_msg_complete
};

enum syslog_retval {
    rv_advance,
    rv_rehash,
    rv_inc_index,
    rv_error
};

typedef enum syslog_state syslog_state;
typedef enum syslog_retval syslog_retval;


// Supporting functions

/**
 * Returns a new pbuffer with a char buffer set to the
 * specified size. This function returns NULL if
 * allocation of the pbuffer or its parts fails.
 */
pbuffer * new_pbuffer(size_t size) {
    // Allocate a new pbuffer struct
    pbuffer *buffer = (pbuffer *) malloc(sizeof(pbuffer));

    if (buffer != NULL) {
        buffer->bytes = (char *) malloc(sizeof(char) * size);

        if (buffer->bytes != NULL) {
            buffer->position = 0;
            buffer->size = size;
        } else {
            // Allocating the actual char buffer failed
            // so release the newly allocated struct
            free(buffer);
            buffer = NULL;
        }
    }

    return buffer;
}

void reset_pbuffer(pbuffer *buffer) {
    buffer->position = 0;
}

void free_pbuffer(pbuffer *buffer) {
    if (buffer->bytes != NULL) {
        free(buffer->bytes);
        buffer->bytes = NULL;
    }

    free(buffer);
}

void free_msg_head(syslog_msg_head *head) {
    if (head->timestamp != NULL) {
        free(head->timestamp);
    }

    if (head->hostname != NULL) {
        free(head->hostname);
    }

    if (head->appname != NULL) {
        free(head->hostname);
    }

    if (head->procid != NULL) {
        free(head->procid);
    }

    if (head->msgid != NULL) {
        free(head->msgid);
    }

    free(head);
}

int store_byte_in_pbuffer(char byte, pbuffer *dest) {
    int retval = 0;

    if (dest-> position + 1 < dest->size) {
        dest->bytes[dest->position] = byte;
        dest->position += 1;
    } else {
        retval = SLERR_BUFFER_OVERFLOW;
    }

    return retval;
}

int copy_into_pbuffer(const char *source, pbuffer *dest, size_t length) {
    int retval = 0;

    if (dest->position + length < dest->size) {
        memcpy(dest->bytes, source, length);
        dest->position += length;
    } else {
        retval = SLERR_BUFFER_OVERFLOW;
    }

    return retval;
}

char * copy_pbuffer(pbuffer *src) {
    char *new = (char *) malloc(src->position * sizeof(char));
    memcpy(new, src->bytes, src->position);

    return new;
}

char * copy_parser_buffer(syslog_parser *parser) {
    return copy_pbuffer(parser->buffer);
}

void reset_buffer(syslog_parser *parser) {
    reset_pbuffer(parser->buffer);
}

int store_byte(char byte, syslog_parser *parser) {
    return store_byte_in_pbuffer(byte, parser->buffer);
}

int on_cb(syslog_parser *parser, syslog_cb cb) {
    return cb(parser);
}

int on_data_cb(syslog_parser *parser, syslog_data_cb cb) {
    return cb(parser, parser->buffer->bytes, parser->buffer->position);
}

void set_token_state(syslog_parser *parser, enum token_state next_state) {
// Print the state switch if we're compiled in DEBUG mode
#if DEBUG_OUTPUT
    printf("Setting token state to: %i", next_state);
#endif

    parser->token_state = next_state;
}

void set_state(syslog_parser *parser, syslog_state next_state) {
// Print the state switch if we're compiled in DEBUG mode
#if DEBUG_OUTPUT
    printf("Setting state to: %i", next_state);
#endif

    parser->state = next_state;
    set_token_state(parser, ts_before);
}

void set_str_field(syslog_parser *parser) {
    char *value = copy_parser_buffer(parser);
    int len = parser->buffer->position;

    switch (parser->state) {
        case s_timestamp:
            parser->msg_head->timestamp = value;
            parser->msg_head->timestamp_len = len;
            break;

        case s_hostname:
            parser->msg_head->hostname = value;
            parser->msg_head->hostname_len = len;
            break;

        case s_appname:
            parser->msg_head->appname = value;
            parser->msg_head->appname_len = len;
            break;

        case s_procid:
            parser->msg_head->procid = value;
            parser->msg_head->procid_len = len;
            break;

        case s_msgid:
            parser->msg_head->msgid = value;
            parser->msg_head->msgid_len = len;
            break;

        default:
            free(value);
    }

    reset_buffer(parser);
}

int read_message(syslog_parser *parser, const syslog_parser_settings *settings, const char *data, size_t length) {
    if (parser->flags & F_COUNT_OCTETS) {
        if (parser->message_length >= length) {
            parser->error = settings->on_msg(parser, data, length);
            parser->read = length;
        } else {
            parser->error = settings->on_msg(parser, data, parser->message_length);
            parser->read = parser->message_length;
        }

        if (parser->message_length == 0) {
            set_state(parser, s_msg_complete);
        }
    } else {
        int stop_idx;

        for (stop_idx = 0; stop_idx < length; stop_idx++) {
            if (data[stop_idx] == '\n') {
                set_state(parser, s_msg_complete);
                break;
            }
        }

        parser->error = settings->on_msg(parser, data, stop_idx);
        parser->read = stop_idx;
    }

    return rv_inc_index;
}

int sd_value(syslog_parser *parser, const syslog_parser_settings *settings, char nb) {
    int retval = rv_advance;

    switch (nb) {
        case '\\':
            parser->flags |= F_ESCAPED;
            break;

        case '"':
            if (parser->flags & F_ESCAPED) {
                parser->flags |= F_ESCAPED;
            } else {
                retval = on_data_cb(parser, settings->on_sd_value);
                set_state(parser, s_sd_field);
            }
            break;

        default:
            store_byte(nb, parser);
    }
}

int sd_value_begin(syslog_parser *parser, char nb) {
    int retval = rv_advance;

    if (nb == '"') {
        set_state(parser, s_sd_value);
    }

    return retval;
}

int sd_field_end(syslog_parser *parser, char nb) {
    int retval = rv_advance;

    if (nb == '=') {
        set_state(parser, s_sd_value_begin);
    }

    return retval;
}

int sd_field(syslog_parser *parser, const syslog_parser_settings *settings, char nb) {
    int retval = rv_advance;

    if (IS_ALPHANUM(nb)) {
        store_byte(nb, parser);
    } else {
        retval = on_data_cb(parser, settings->on_sd_field);

        switch (nb) {
            case ']':
                set_state(parser, s_sd_start);
                break;

            case '=':
                set_state(parser, s_sd_value_begin);
                break;

            default:
                set_state(parser, s_sd_field_end);
        }
    }

    return retval;
}

int sd_element(syslog_parser *parser, const syslog_parser_settings *settings, char nb) {
    int retval = rv_advance;

    if (!IS_WS(nb)) {
        store_byte(nb, parser);
    } else {
        retval = on_data_cb(parser, settings->on_sd_element);
        set_state(parser, s_sd_field);
    }

    return retval;
}

int sd_start(syslog_parser *parser, const syslog_parser_settings *settings, char nb) {
    int retval = rv_advance;

    parser->error = on_cb(parser, settings->on_msg_head);

    switch (nb) {
        case '[':
            set_state(parser, s_sd_element);
            break;

        case '-':
            set_state(parser, s_message);
            break;

        default:
            set_state(parser, s_message);
            retval = rv_rehash;
    }

    return retval;
}

int parse_msg_head_part(syslog_parser *parser, syslog_state next_state, char nb) {
    if (!IS_WS(nb)) {
        store_byte(nb, parser);
    } else {
        set_str_field(parser);
        set_state(parser, next_state);
    }

    return rv_advance;
}

int version(syslog_parser *parser, char nb) {
    int retval = rv_advance;

    if (IS_NUM(nb)) {
        uint8_t nversion = parser->message_length;
        nversion *= 10;
        nversion += nb - '0';

        if (nversion < parser->msg_head->version|| nversion > 999) {
            parser->error = SLERR_BAD_VERSION;
        } else {
            parser->msg_head->version = nversion;
        }
    } else {
        set_state(parser, s_timestamp);
    }

    return retval;
}

int priority(syslog_parser *parser, char nb) {
    if (IS_NUM(nb)) {
        uint8_t npri = parser->message_length;
        npri *= 10;
        npri += nb - '0';

        if (npri < parser->msg_head->pri || npri > 999) {
            parser->error = SLERR_BAD_PRIORITY;
        } else {
            parser->msg_head->pri = npri;
        }
    } else {
        switch (nb) {
            case '>':
                set_state(parser, s_version);
                break;

            default:
                parser->error = SLERR_BAD_PRIORITY;
        }
    }

    return rv_advance;
}

int priority_start(syslog_parser *parser, char nb) {
    switch(nb) {
        case '<':
            set_state(parser, s_priority);
            break;

        default:
            parser->error = SLERR_BAD_PRIORITY_START;
    }

    return rv_advance;
}

int octet_count(syslog_parser *parser, char nb) {
    if (IS_NUM(nb)) {
        uint64_t mlength = parser->message_length;

        mlength *= 10;
        mlength += nb - '0';

        if (mlength < parser->message_length || mlength == ULONG_MAX) {
            parser->error = SLERR_BAD_OCTET_COUNT;
        } else {
            parser->message_length = mlength;
        }
    } else {
        set_state(parser, s_priority_start);
    }

    return rv_advance;
}

int msg_start(syslog_parser *parser, const syslog_parser_settings *settings, char nb) {
    if (IS_NUM(nb)) {
        parser->flags |= F_COUNT_OCTETS;
        set_state(parser, s_octet_count);
    } else {
        set_state(parser, s_priority);
    }

    return rv_rehash;
}

// Big state switch
int uslg_parser_exec(syslog_parser *parser, const syslog_parser_settings *settings, const char *data, size_t length) {
    int action, d_index;
    bool exit_exec = false;

    // Continue in the loop as long as we're told not to exit and there's data to chew on
    for (d_index = 0; !exit_exec && d_index < length; d_index++) {
        char next_byte = data[d_index];

#if DEBUG_OUTPUT
        // Get the next character being processed during debug
        printf("Next byte: %c\n", next_byte);
#endif

        // Token state is managed first
        if (parser->token_state == ts_before) {
            switch (next_byte) {
                case ' ':
                case '\t':
                    // Decrease the message length if we're counting octets
                    if (parser->flags & F_COUNT_OCTETS) {
                        parser->message_length--;
                    }
                    continue;

                default:
                    set_token_state(parser, ts_read);
            }
        }

        switch (parser->state) {
            case s_msg_start:
                action = msg_start(parser, settings, next_byte);
                break;

            case s_octet_count:
                action = octet_count(parser, next_byte);
                break;

            case s_priority_start:
                action = priority_start(parser, next_byte);
                break;

            case s_priority:
                action = priority(parser, next_byte);
                break;

            case s_version:
                action = version(parser, next_byte);
                break;

            case s_timestamp:
                action = parse_msg_head_part(parser, s_hostname, next_byte);
                break;

            case s_hostname:
                action = parse_msg_head_part(parser, s_appname, next_byte);
                break;

            case s_appname:
                action = parse_msg_head_part(parser, s_procid, next_byte);
                break;

            case s_procid:
                action = parse_msg_head_part(parser, s_msgid, next_byte);
                break;

            case s_msgid:
                action = parse_msg_head_part(parser, s_sd_start, next_byte);
                break;

            case s_sd_start:
                action = sd_start(parser, settings, next_byte);
                break;

            case s_sd_element:
                action = sd_element(parser, settings, next_byte);
                break;

            case s_sd_field:
                action = sd_field(parser, settings, next_byte);
                break;

            case s_sd_field_end:
                action = sd_field_end(parser, next_byte);
                break;

            case s_sd_value_begin:
                action = sd_value_begin(parser, next_byte);
                break;

            case s_sd_value:
                action = sd_value(parser, settings, next_byte);
                break;

            case s_message:
                action = read_message(parser, settings, data + d_index, length - d_index);
                break;

            default:
                parser->error = SLERR_BAD_STATE;
        }

        if (parser->error) {
            // An error occured
            exit_exec = true;
        }

        switch (action) {
            case rv_advance:
                if (parser->state == s_msg_complete) {
                    parser->error = on_cb(parser, settings->on_msg_complete);
                    uslg_parser_reset(parser);
                }
                break;

            case rv_inc_index:
                d_index += parser->read;
                break;

            case rv_rehash:
                d_index--;
                break;

            default:
                exit_exec = true;
        }
    }

    return parser->error;
}


// Exported Functions

void uslg_parser_reset(syslog_parser *parser) {
    parser->read = 0;
    parser->error = 0;
    parser->flags = 0;
    parser->message_length = 0;

    parser->state = s_msg_start;
    parser->token_state = ts_before;
}

int uslg_parser_init(syslog_parser *parser, void *app_data) {
    memset(parser, 0, sizeof(*parser));

    // Create the msg_head
    parser->msg_head = (syslog_msg_head *) malloc(sizeof(syslog_msg_head));

    if (errno) {
        // Allocating the msg_head struct failed!
        return -1;
    }

    memset(parser->msg_head, 0, sizeof(syslog_msg_head));

    parser->app_data = app_data;
    parser->buffer = new_pbuffer(MAX_BUFFER_SIZE);

    if (errno) {
        // Allocating the buffer failed so let go
        // of the memory we just allocated for the
        // msg_head struct
        free_msg_head(parser->msg_head);
        parser->msg_head = NULL;
        return -1;
    }

    uslg_parser_reset(parser);
    return 0;
}

void uslg_free_parser(syslog_parser *parser) {
    free_msg_head(parser->msg_head);
    free_pbuffer(parser->buffer);
    free(parser);
}