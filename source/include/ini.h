/* inih -- simple .INI file parser

SPDX-License-Identifier: BSD-3-Clause

Copyright (C) 2009-2020, Ben Hoyt

inih is released under the New BSD license (see LICENSE.txt). Go to the project
home page for more info:

https://github.com/benhoyt/inih

*/

#ifndef INI_H
#define INI_H

#include <stdio.h>
#include <unistd.h>

/* Make this header file easier to include in C++ code */
#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LINE 256
#define MAX_PAIRS 100

typedef struct {
    char key[MAX_LINE];
    char value[MAX_LINE];
} KeyValue;

typedef struct {
    KeyValue pairs[MAX_PAIRS];
    int count;
} IniParser;


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>

static inline void trim(char* str) {
    char* start = str;
    while (isspace((unsigned char)*start)) start++;

    char* end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;

    const size_t length = (size_t)(end - start);
    if (start != str && length > 0) {
        memmove(str, start, length);
    }
    str[length] = '\0';
}
static inline char* my_strchr(const char* str, int c) {
    while (*str != '\0') {
        if (*str == (char)c) {
            return (char*)str;
        }
        str++;
    }
    return NULL;
}
static inline int ini_parser_load(IniParser* parser, const char* filename) {
    if (parser == NULL || filename == NULL) {
        return 0;
    }

    int fd = open(filename, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    parser->count = 0;

    char buffer[MAX_LINE];
    ssize_t bytesRead;
    int bufferIndex = 0;
    char section[MAX_LINE] = "";

    while ((bytesRead = read(fd, buffer + bufferIndex, sizeof(buffer) - bufferIndex - 1)) > 0) {
        buffer[bytesRead + bufferIndex] = '\0';
        bufferIndex += bytesRead;
        char* line = buffer;
        char* lineEnd;

        //shellui_log("Buffer: %s", buffer);

        while ((lineEnd = my_strchr(line, '\n')) != NULL) {
            *lineEnd = '\0';
            char trimmedLine[MAX_LINE];
            strncpy(trimmedLine, line, sizeof(trimmedLine) - 1);
            trimmedLine[sizeof(trimmedLine) - 1] = '\0';
            trim(trimmedLine);

            if (trimmedLine[0] == ';' || trimmedLine[0] == '#' || trimmedLine[0] == '\0') {
                // Skip comments and empty lines
            }
            else if (trimmedLine[0] == '[' && trimmedLine[strlen(trimmedLine) - 1] == ']') {
                // Section header
                const size_t sectionLength = strlen(trimmedLine) - 2;
                if (sectionLength >= sizeof(section)) {
                    line = lineEnd + 1;
                    continue;
                }
                memcpy(section, trimmedLine + 1, sectionLength);
                section[sectionLength] = '\0';
                trim(section);
            }
            else {
                // Key-value pair
                char* delimiter = my_strchr(trimmedLine, '=');
                if (delimiter) {
                    *delimiter = '\0';
                    char key[MAX_LINE];
                    const int keyLength = snprintf(key, sizeof(key), "%s.%s",
                                                   section, trimmedLine);
                    if (keyLength < 0 || (size_t)keyLength >= sizeof(key)) {
                        line = lineEnd + 1;
                        continue;
                    }
                    trim(key);
                    char* value = delimiter + 1;
                    trim(value);

                    if (parser->count < MAX_PAIRS) {
                        strncpy(parser->pairs[parser->count].key, key, MAX_LINE);
                        strncpy(parser->pairs[parser->count].value, value, MAX_LINE);
                        parser->count++;
                    }
                }
            }

            line = lineEnd + 1;
        }

        bufferIndex = strlen(line);
        memmove(buffer, line, bufferIndex);
        if (bufferIndex == sizeof(buffer) - 1) {
            close(fd);
            parser->count = 0;
            return 0;
        }
    }

    close(fd);
    return 1;
}


static inline const char* ini_parser_get(IniParser* parser, const char* key, const char* default_value) {
    for (int i = 0; i < parser->count; i++) {
        if (strcmp(parser->pairs[i].key, key) == 0) {
            return parser->pairs[i].value;
        }
    }
    return default_value;
}


#ifdef __cplusplus
}
#endif

#endif /* INI_H */
