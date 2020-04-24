/*
 Copyright (c) 2016 Mathieu Laurendeau <mat.lau@laposte.net>
 License: GPLv3
 */

#include <gpoll.h>

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <gimxcommon/include/gerror.h>
#include <gimxcommon/include/glist.h>
#include <gimxlog/include/glog.h>

#define MAX_SOURCES (MAXIMUM_WAIT_OBJECTS-1)

GLOG_INST(GLOG_NAME)

struct poll_source
{
  int fd;
  void * user;
  HANDLE handle;
  int (*fp_read)(void * user);
  int (*fp_write)(void * user);
  int (*fp_cleanup)(void * user);
  int removed;
  GLIST_LINK(struct poll_source);
};

static unsigned int nb_sources = 0;

static int polling = 0;

#define CHECK_NB_SOURCES() \
    do { \
        if (nb_sources == MAX_SOURCES) { \
            PRINT_ERROR_OTHER("max number of sources reached"); \
            return -1; \
        } \
    } while (0)

static GLIST_INST(struct poll_source, sources);

static void gpoll_close_internal(struct poll_source * source) {

    if (source->fd >= 0) {
        WSACloseEvent(source->handle);
    }

    GLIST_REMOVE(sources, source);
    free(source);
    --nb_sources;
}

static int (*rawinput_callback)() = NULL;

/*
 * Register a socket as an event source.
 * Note that the socket becomes non-blocking.
 */
int gpoll_register_fd(int fd, void * user, const GPOLL_CALLBACKS * callbacks) {

    if (fd < 0) {
        PRINT_ERROR_OTHER("fd is invalid");
        return -1;
    }
    if (!callbacks->fp_close) {
        PRINT_ERROR_OTHER("fp_close is mandatory");
        return -1;
    }
    if (!callbacks->fp_read) {
        PRINT_ERROR_OTHER("fp_read is NULL");
        return -1;
    }
    CHECK_NB_SOURCES();

    struct poll_source * source = calloc(1, sizeof(*source));
    if (source == NULL) {
        PRINT_ERROR_ALLOC_FAILED("calloc");
        return -1;
    }

    HANDLE evt = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (WSAEventSelect(fd, evt, FD_READ | FD_CLOSE) == SOCKET_ERROR) {
        PRINT_ERROR_OTHER("WSAEventSelect failed.");
        return -1;
    }

    source->fd = fd;
    source->user = user;
    source->handle = evt;
    source->fp_read = callbacks->fp_read;
    source->fp_cleanup = callbacks->fp_close;

    GLIST_ADD(sources, source);
    ++nb_sources;

    return 0;
}

int gpoll_register_handle(HANDLE handle, void * user, const GPOLL_CALLBACKS * callbacks) {

    if (handle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR_OTHER("handle is invalid");
        return -1;
    }
    if (!callbacks->fp_close) {
        PRINT_ERROR_OTHER("fp_close is mandatory");
        return -1;
    }
    if (!callbacks->fp_read && !callbacks->fp_write) {
        PRINT_ERROR_OTHER("fp_read and fp_write are NULL");
        return -1;
    }
    CHECK_NB_SOURCES();

    struct poll_source * source = calloc(1, sizeof(*source));
    if (source == NULL) {
        PRINT_ERROR_ALLOC_FAILED("calloc");
        return -1;
    }

    source->fd = -1;
    source->user = user;
    source->handle = handle;
    source->fp_read = callbacks->fp_read;
    source->fp_write = callbacks->fp_write;
    source->fp_cleanup = callbacks->fp_close;

    GLIST_ADD(sources, source);
    ++nb_sources;

    return 0;
}

int gpoll_remove_handle(HANDLE handle) {

    struct poll_source * current;
    for (current = GLIST_BEGIN(sources); current != GLIST_END(sources); current = current->next) {
        if (handle == current->handle) {
            if (polling) {
                current->removed = 1;
            } else {
                gpoll_close_internal(current);
            }
            return 0;
        }
    }
    return -1;
}

int gpoll_remove_fd(int fd) {

    struct poll_source * current;
    for (current = GLIST_BEGIN(sources); current != GLIST_END(sources); current = current->next) {
        if (fd == current->fd) {
            if (polling) {
                current->removed = 1;
            } else {
                gpoll_close_internal(current);
            }
            return 0;
        }
    }
    return -1;
}

static void gpoll_remove_deferred() {

    struct poll_source * current;
    for (current = GLIST_BEGIN(sources); current != GLIST_END(sources); current = current->next) {
        if (current->removed) {
            struct poll_source * prev = current->prev;
            gpoll_close_internal(current);
            current = prev;
        }
    }
}

static unsigned int fill_handles(unsigned int nhandles, HANDLE handles[nhandles]) {

    unsigned int pos = 0;
    struct poll_source * current;
    for (current = GLIST_BEGIN(sources); current != GLIST_END(sources) && pos < nhandles; current = current->next) {
        handles[pos] = current->handle;
        ++pos;
    }
    return pos;
}

void gpoll() {

    polling = 1;

    DWORD result;
    int done = 0;
    int rawinput_processed = 0;

    do {
        HANDLE handles[nb_sources];
        DWORD count = fill_handles(nb_sources, handles);

        unsigned int dwWakeMask = 0;

        if (rawinput_callback != NULL) {
            dwWakeMask = QS_RAWINPUT;
        }

        result = MsgWaitForMultipleObjects(count, handles, FALSE, INFINITE, dwWakeMask);

        if (result == WAIT_FAILED) {
            PRINT_ERROR_GETLASTERROR("MsgWaitForMultipleObjects");
            continue;
        }

        /*
         * Check the state of every object so as to prevent starvation.
         */

        if (rawinput_callback != NULL) {
            if (GetQueueStatus(QS_RAWINPUT)) {
                rawinput_processed = 1;
                if (rawinput_callback()) {
                    done = 1;
                }
            }
        }

        struct poll_source * current;
        for (current = GLIST_BEGIN(sources); current != GLIST_END(sources); current = current->next) {

            if (current->fp_read == NULL && current->fp_write == NULL) {
                continue;
            }
            if (result >= count || current->handle != handles[result]) {
                /*
                 * Check every object except the one that has been signaled.
                 */
                DWORD lresult = WaitForSingleObject(current->handle, 0);
                if (lresult == WAIT_FAILED) {
                    PRINT_ERROR_GETLASTERROR("WaitForSingleObject");
                    continue;
                } else if (lresult != WAIT_OBJECT_0) {
                    continue;
                }
            }
            if (current->fd >= 0) {
                WSANETWORKEVENTS NetworkEvents;
                /*
                 * Network source
                 */
                if (WSAEnumNetworkEvents(current->fd, current->handle, &NetworkEvents)) {
                    PRINT_ERROR_GETLASTERROR("WSAEnumNetworkEvents");
                    current->fp_cleanup(current->user);
                } else {
                    if (NetworkEvents.lNetworkEvents & FD_READ) {
                        if (NetworkEvents.iErrorCode[FD_READ_BIT]) {
                            PRINT_ERROR_OTHER("iErrorCode[FD_READ_BIT] is set");
                            current->fp_cleanup(current->user);
                        } else {
                            if (current->fp_read(current->user)) {
                                done = 1;
                            }
                        }
                    }
                }
            } else {
                /*
                 * Other sources (timers, COM port, HID...)
                 */
                if (current->fp_read != NULL) {
                    if (current->fp_read(current->user)) {
                        done = 1;
                    }
                }
                if (current->fp_write != NULL) {
                    if (current->fp_write(current->user)) {
                        done = 1;
                    }
                }
            }
        }

        gpoll_remove_deferred();

    } while (!done);

    /*
     * If window messages are not processed after 5s the app will be considered unresponsive,
     * and captured mouse pointer will be released if task manger is running.
     * Make sure non-rawinput messages are still processed when there is no rawinput message.
     * This assumes a timer with a period lower than 5s was registered.
     */

    if (rawinput_processed == 0 && rawinput_callback != NULL) {
        if (rawinput_callback()) {
            done = 1;
        }
    }

    polling = 0;
}

void gpoll_set_rawinput_callback(int (*callback)()) {

    rawinput_callback = callback;
}
