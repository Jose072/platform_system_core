/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <syslog.h>

#include <log/logger.h>

#include "LogKlog.h"

#define KMSG_PRIORITY(PRI)           \
    '<',                             \
    '0' + (LOG_SYSLOG | (PRI)) / 10, \
    '0' + (LOG_SYSLOG | (PRI)) % 10, \
    '>'

static const char priority_message[] = { KMSG_PRIORITY(LOG_INFO), '\0' };

log_time LogKlog::correction = log_time(CLOCK_REALTIME) - log_time(CLOCK_MONOTONIC);

LogKlog::LogKlog(LogBuffer *buf, LogReader *reader, int fdWrite, int fdRead, bool auditd) :
        SocketListener(fdRead, false),
        logbuf(buf),
        reader(reader),
        signature(CLOCK_MONOTONIC),
        fdWrite(fdWrite),
        fdRead(fdRead),
        initialized(false),
        enableLogging(true),
        auditd(auditd) {
    static const char klogd_message[] = "%slogd.klogd: %" PRIu64 "\n";
    char buffer[sizeof(priority_message) + sizeof(klogd_message) + 20 - 4];
    snprintf(buffer, sizeof(buffer), klogd_message, priority_message,
        signature.nsec());
    write(fdWrite, buffer, strlen(buffer));
}

bool LogKlog::onDataAvailable(SocketClient *cli) {
    if (!initialized) {
        prctl(PR_SET_NAME, "logd.klogd");
        initialized = true;
        enableLogging = false;
    }

    char buffer[LOGGER_ENTRY_MAX_PAYLOAD];
    size_t len = 0;

    for(;;) {
        ssize_t retval = 0;
        if ((sizeof(buffer) - 1 - len) > 0) {
            retval = read(cli->getSocket(), buffer + len, sizeof(buffer) - 1 - len);
        }
        if ((retval == 0) && (len == 0)) {
            break;
        }
        if (retval < 0) {
            return false;
        }
        len += retval;
        bool full = len == (sizeof(buffer) - 1);
        char *ep = buffer + len;
        *ep = '\0';
        len = 0;
        for(char *ptr, *tok = buffer;
                ((tok = strtok_r(tok, "\r\n", &ptr)));
                tok = NULL) {
            if (((tok + strlen(tok)) == ep) && (retval != 0) && full) {
                len = strlen(tok);
                memmove(buffer, tok, len);
                break;
            }
            if (*tok) {
                log(tok);
            }
        }
    }

    return true;
}


void LogKlog::calculateCorrection(const log_time &monotonic,
                                  const char *real_string) {
    log_time real;
    if (!real.strptime(real_string, "%Y-%m-%d %H:%M:%S.%09q UTC")) {
        return;
    }
    // kernel report UTC, log_time::strptime is localtime from calendar.
    // Bionic and liblog strptime does not support %z or %Z to pick up
    // timezone so we are calculating our own correction.
    time_t now = real.tv_sec;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_isdst = -1;
    localtime_r(&now, &tm);
    real.tv_sec += tm.tm_gmtoff;
    correction = real - monotonic;
}

void LogKlog::sniffTime(log_time &now, const char **buf, bool reverse) {
    const char *cp;
    if ((cp = now.strptime(*buf, "[ %s.%q]"))) {
        static const char suspend[] = "PM: suspend entry ";
        static const char resume[] = "PM: suspend exit ";
        static const char suspended[] = "Suspended for ";

        if (isspace(*cp)) {
            ++cp;
        }
        if (!strncmp(cp, suspend, sizeof(suspend) - 1)) {
            calculateCorrection(now, cp + sizeof(suspend) - 1);
        } else if (!strncmp(cp, resume, sizeof(resume) - 1)) {
            calculateCorrection(now, cp + sizeof(resume) - 1);
        } else if (!strncmp(cp, suspended, sizeof(suspended) - 1)) {
            log_time real;
            char *endp;
            real.tv_sec = strtol(cp + sizeof(suspended) - 1, &endp, 10);
            if (*endp == '.') {
                real.tv_nsec = strtol(endp + 1, &endp, 10) * 1000000L;
                if (reverse) {
                    correction -= real;
                } else {
                    correction += real;
                }
            }
        }

        convertMonotonicToReal(now);
        *buf = cp;
    } else {
        now = log_time(CLOCK_REALTIME);
    }
}

// Passed the entire SYSLOG_ACTION_READ_ALL buffer and interpret a
// compensated start time.
void LogKlog::synchronize(const char *buf) {
    const char *cp = strstr(buf, "] PM: suspend e");
    if (!cp) {
        return;
    }

    do {
        --cp;
    } while ((cp > buf) && (isdigit(*cp) || isspace(*cp) || (*cp == '.')));

    log_time now;
    sniffTime(now, &cp, true);

    char *suspended = strstr(buf, "] Suspended for ");
    if (!suspended || (suspended > cp)) {
        return;
    }
    cp = suspended;

    do {
        --cp;
    } while ((cp > buf) && (isdigit(*cp) || isspace(*cp) || (*cp == '.')));

    sniffTime(now, &cp, true);
}

// kernel log prefix, convert to a kernel log priority number
static int parseKernelPrio(const char **buf) {
    int pri = LOG_USER | LOG_INFO;
    const char *cp = *buf;
    if (*cp == '<') {
        pri = 0;
        while(isdigit(*++cp)) {
            pri = (pri * 10) + *cp - '0';
        }
        if (*cp == '>') {
            ++cp;
        } else {
            cp = *buf;
            pri = LOG_USER | LOG_INFO;
        }
        *buf = cp;
    }
    return pri;
}

// Convert kernel log priority number into an Android Logger priority number
static int convertKernelPrioToAndroidPrio(int pri) {
    switch(pri & LOG_PRIMASK) {
    case LOG_EMERG:
        // FALLTHRU
    case LOG_ALERT:
        // FALLTHRU
    case LOG_CRIT:
        return ANDROID_LOG_FATAL;

    case LOG_ERR:
        return ANDROID_LOG_ERROR;

    case LOG_WARNING:
        return ANDROID_LOG_WARN;

    default:
        // FALLTHRU
    case LOG_NOTICE:
        // FALLTHRU
    case LOG_INFO:
        break;

    case LOG_DEBUG:
        return ANDROID_LOG_DEBUG;
    }

    return ANDROID_LOG_INFO;
}

//
// log a message into the kernel log buffer
//
// Filter rules to parse <PRI> <TIME> <tag> and <message> in order for
// them to appear correct in the logcat output:
//
// LOG_KERN (0):
// <PRI>[<TIME>] <tag> ":" <message>
// <PRI>[<TIME>] <tag> <tag> ":" <message>
// <PRI>[<TIME>] <tag> <tag>_work ":" <message>
// <PRI>[<TIME>] <tag> '<tag>.<num>' ":" <message>
// <PRI>[<TIME>] <tag> '<tag><num>' ":" <message>
// <PRI>[<TIME>] <tag>_host '<tag>.<num>' ":" <message>
// (unimplemented) <PRI>[<TIME>] <tag> '<num>.<tag>' ":" <message>
// <PRI>[<TIME>] "[INFO]"<tag> : <message>
// <PRI>[<TIME>] "------------[ cut here ]------------"   (?)
// <PRI>[<TIME>] "---[ end trace 3225a3070ca3e4ac ]---"   (?)
// LOG_USER, LOG_MAIL, LOG_DAEMON, LOG_AUTH, LOG_SYSLOG, LOG_LPR, LOG_NEWS
// LOG_UUCP, LOG_CRON, LOG_AUTHPRIV, LOG_FTP:
// <PRI+TAG>[<TIME>] (see sys/syslog.h)
// Observe:
//  Minimum tag length = 3   NB: drops things like r5:c00bbadf, but allow PM:
//  Maximum tag words = 2
//  Maximum tag length = 16  NB: we are thinking of how ugly logcat can get.
//  Not a Tag if there is no message content.
//  leading additional spaces means no tag, inherit last tag.
//  Not a Tag if <tag>: is "ERROR:", "WARNING:", "INFO:" or "CPU:"
// Drop:
//  empty messages
//  messages with ' audit(' in them if auditd is running
//  logd.klogd:
// return -1 if message logd.klogd: <signature>
//
int LogKlog::log(const char *buf) {
    if (auditd && strstr(buf, " audit(")) {
        return 0;
    }

    int pri = parseKernelPrio(&buf);

    log_time now;
    sniffTime(now, &buf, false);

    // sniff for start marker
    const char klogd_message[] = "logd.klogd: ";
    if (!strncmp(buf, klogd_message, sizeof(klogd_message) - 1)) {
        char *endp;
        uint64_t sig = strtoll(buf + sizeof(klogd_message) - 1, &endp, 10);
        if (sig == signature.nsec()) {
            if (initialized) {
                enableLogging = true;
            } else {
                enableLogging = false;
            }
            return -1;
        }
        return 0;
    }

    if (!enableLogging) {
        return 0;
    }

    // Parse pid, tid and uid (not possible)
    const pid_t pid = 0;
    const pid_t tid = 0;
    const uid_t uid = 0;

    // Parse (rules at top) to pull out a tag from the incoming kernel message.
    // Some may view the following as an ugly heuristic, the desire is to
    // beautify the kernel logs into an Android Logging format; the goal is
    // admirable but costly.
    while (isspace(*buf)) {
        ++buf;
    }
    if (!*buf) {
        return 0;
    }
    const char *start = buf;
    const char *tag = "";
    const char *etag = tag;
    if (!isspace(*buf)) {
        const char *bt, *et, *cp;

        bt = buf;
        if (!strncmp(buf, "[INFO]", 6)) {
            // <PRI>[<TIME>] "[INFO]"<tag> ":" message
            bt = buf + 6;
        }
        for(et = bt; *et && (*et != ':') && !isspace(*et); ++et);
        for(cp = et; isspace(*cp); ++cp);
        size_t size;

        if (*cp == ':') {
            // One Word
            tag = bt;
            etag = et;
            buf = cp + 1;
        } else {
            size = et - bt;
            if (strncmp(bt, cp, size)) {
                // <PRI>[<TIME>] <tag>_host '<tag>.<num>' : message
                if (!strncmp(bt + size - 5, "_host", 5)
                 && !strncmp(bt, cp, size - 5)) {
                    const char *b = cp;
                    cp += size - 5;
                    if (*cp == '.') {
                        while (!isspace(*++cp) && (*cp != ':'));
                        const char *e;
                        for(e = cp; isspace(*cp); ++cp);
                        if (*cp == ':') {
                            tag = b;
                            etag = e;
                            buf = cp + 1;
                        }
                    }
                } else {
                    while (!isspace(*++cp) && (*cp != ':'));
                    const char *e;
                    for(e = cp; isspace(*cp); ++cp);
                    // Two words
                    if (*cp == ':') {
                        tag = bt;
                        etag = e;
                        buf = cp + 1;
                    }
                }
            } else if (isspace(cp[size])) {
                const char *b = cp;
                cp += size;
                while (isspace(*++cp));
                // <PRI>[<TIME>] <tag> <tag> : message
                if (*cp == ':') {
                    tag = bt;
                    etag = et;
                    buf = cp + 1;
                }
            } else if (cp[size] == ':') {
                // <PRI>[<TIME>] <tag> <tag> : message
                tag = bt;
                etag = et;
                buf = cp + size + 1;
            } else if ((cp[size] == '.') || isdigit(cp[size])) {
                // <PRI>[<TIME>] <tag> '<tag>.<num>' : message
                // <PRI>[<TIME>] <tag> '<tag><num>' : message
                const char *b = cp;
                cp += size;
                while (!isspace(*++cp) && (*cp != ':'));
                const char *e = cp;
                while (isspace(*cp)) {
                    ++cp;
                }
                if (*cp == ':') {
                    tag = b;
                    etag = e;
                    buf = cp + 1;
                }
            } else {
                while (!isspace(*++cp) && (*cp != ':'));
                const char *e = cp;
                while (isspace(*cp)) {
                    ++cp;
                }
                // Two words
                if (*cp == ':') {
                    tag = bt;
                    etag = e;
                    buf = cp + 1;
                }
            }
        }
        size = etag - tag;
        if ((size <= 1)
         || ((size == 2) && (isdigit(tag[0]) || isdigit(tag[1])))
         || ((size == 3) && !strncmp(tag, "CPU", 3))
         || ((size == 7) && !strncmp(tag, "WARNING", 7))
         || ((size == 5) && !strncmp(tag, "ERROR", 5))
         || ((size == 4) && !strncmp(tag, "INFO", 4))) {
            buf = start;
            etag = tag = "";
        }
    }
    size_t l = etag - tag;
    while (isspace(*buf)) {
        ++buf;
    }
    size_t n = 1 + l + 1 + strlen(buf) + 1;

    // Allocate a buffer to hold the interpreted log message
    int rc = n;
    char *newstr = reinterpret_cast<char *>(malloc(n));
    if (!newstr) {
        rc = -ENOMEM;
        return rc;
    }
    char *np = newstr;

    // Convert priority into single-byte Android logger priority
    *np = convertKernelPrioToAndroidPrio(pri);
    ++np;

    // Copy parsed tag following priority
    strncpy(np, tag, l);
    np += l;
    *np = '\0';
    ++np;

    // Copy main message to the remainder
    strcpy(np, buf);

    // Log message
    rc = logbuf->log(LOG_ID_KERNEL, now, uid, pid, tid, newstr,
                     (n <= USHRT_MAX) ? (unsigned short) n : USHRT_MAX);
    free(newstr);

    // notify readers
    if (!rc) {
        reader->notifyNewLog();
    }

    return rc;
}
