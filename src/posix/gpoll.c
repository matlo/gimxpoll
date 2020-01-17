/*
 Copyright (c) 2016 Mathieu Laurendeau <mat.lau@laposte.net>
 License: GPLv3
 */

#include <gpoll.h>
#include <gimxcommon/include/gerror.h>
#include <gimxcommon/include/glist.h>
#include <gimxlog/include/glog.h>

#include <stdio.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

GLOG_INST(GLOG_NAME)

struct poll_source {
    int fd;
    void * user;
    int (*fp_read)(void * user);
    int (*fp_write)(void * user);
    int (*fp_close)(void * user);
    int events;
    int removed;
    GLIST_LINK(struct poll_source);
};

static unsigned int nb_sources = 0;

static int polling = 0;

static GLIST_INST(struct poll_source, sources);

static void gpoll_close_internal(struct poll_source * source) {

    GLIST_REMOVE(sources, source);
    free(source);
    --nb_sources;
}

int gpoll_register_fd(int fd, void * user, const GPOLL_CALLBACKS * callbacks) {

    if (!callbacks->fp_close) {
        PRINT_ERROR_OTHER("fp_close is mandatory");
        return -1;
    }
    if (!callbacks->fp_read && !callbacks->fp_write) {
        PRINT_ERROR_OTHER("fp_read and fp_write are NULL");
        return -1;
    }
    struct poll_source * source = calloc(1, sizeof(*source));
    if (source == NULL) {
        PRINT_ERROR_ALLOC_FAILED("calloc");
        return -1;
    }
    source->fd = fd;
    source->user = user;
    source->fp_read = callbacks->fp_read;
    if (source->fp_read != NULL) {
        source->events |= POLLIN;
    }
    source->fp_write = callbacks->fp_write;
    if (source->fp_write != NULL) {
        source->events |= POLLOUT;
    }
    source->fp_close = callbacks->fp_close;
    GLIST_ADD(sources, source);
    ++nb_sources;
    return 0;
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

static unsigned int fill_fds(nfds_t nfds, struct pollfd fds[nfds], struct poll_source * sources[nfds]) {

    unsigned int pos = 0;

    struct poll_source * current;
    for (current = GLIST_BEGIN(sources); current != GLIST_END(sources) && pos < nfds; current = current->next) {
        fds[pos].fd = current->fd;
        fds[pos].events = current->events;
        sources[pos] = current;
        ++pos;
    }

    return pos;
}

static void gpoll_remove_fd_defered() {

    struct poll_source * current;
    for (current = GLIST_BEGIN(sources); current != GLIST_END(sources); current = current->next) {
        if (current->removed) {
            struct poll_source * prev = current->prev;
            gpoll_close_internal(current);
            current = prev;
        }
    }
}

void gpoll(void) {

    polling = 1;

    unsigned int i;
    int res;

    while (1) {

        struct pollfd fds[nb_sources];
        struct poll_source * sources[nb_sources];
        nfds_t nfds = fill_fds(nb_sources, fds, sources);

        if (poll(fds, nfds, -1) > 0) {
            for (i = 0; i < nfds; ++i) {
                if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    res = sources[i]->fp_close(sources[i]->user);
                    gpoll_remove_fd(fds[i].fd);
                    if (res) {
                        break;
                    }
                    continue;
                }
                if (fds[i].revents & POLLIN) {
                    if (sources[i]->fp_read(sources[i]->user)) {
                        break;
                    }
                }
                if (fds[i].revents & POLLOUT) {
                    if (sources[i]->fp_write(sources[i]->user)) {
                        break;
                    }
                }
            }
            gpoll_remove_fd_defered();
            if (i < nfds) {
                break;
            }

        } else {
            if (errno != EINTR) {
                PRINT_ERROR_ERRNO("poll");
            }
        }
    }

    polling = 0;
}
