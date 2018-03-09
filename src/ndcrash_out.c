#include "ndcrash.h"
#include "ndcrash_out.h"
#include "ndcrash_private.h"
#include "ndcrash_signal_utils.h"
#include "ndcrash_log.h"
#include <signal.h>
#include <malloc.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/socket.h>
#include <linux/un.h>

#ifdef ENABLE_OUTOFPROCESS

struct ndcrash_out_context {

    /// Old handlers of signals that we restore on de-initialization. Keep values for all possible
    /// signals, for unused signals NULL value is stored.
    struct sigaction old_handlers[NSIG];
};

/// Global instance of out-of-process context.
struct ndcrash_out_context *ndcrash_out_context_instance = NULL;

/// Signal handling function for out-of-process architecture.
void ndcrash_out_signal_handler(int signo, struct siginfo *siginfo, void *ctxvoid) {
    // Restoring an old handler to make built-in Android crash mechanism work.
    sigaction(signo, &ndcrash_out_context_instance->old_handlers[signo], NULL);

    // Filling message fields.
    struct ndcrash_out_message msg;
    msg.pid = getpid();
    msg.tid = gettid();
    msg.signo = signo;
    msg.si_code = siginfo->si_code;
    msg.faultaddr = siginfo->si_addr;
    memcpy(&msg.context, ctxvoid, sizeof(struct ucontext));

    __android_log_print(
            ANDROID_LOG_ERROR,
            NDCRASH_LOG_TAG,
            "Signal caught: %d (%s), code %d (%s) pid: %d, tid: %d",
            signo,
            ndcrash_get_signame(signo),
            siginfo->si_code,
            ndcrash_get_sigcode(signo, siginfo->si_code),
            msg.pid,
            msg.tid);

    // Connecting to service using UNIX domain socket, sending message to it.
    // Using blocking sockets!
    const int sock = socket(PF_LOCAL, SOCK_STREAM, 0);
    if (sock < 0) {
        __android_log_print(ANDROID_LOG_ERROR, NDCRASH_LOG_TAG, "Couldn't create socket, error: %s (%d)", strerror(errno), errno);
        return;
    }

    // Discarding terminating \0 char.
    const size_t socket_name_size = sizeofa(SOCKET_NAME) - 1;

    // Setting socket address.
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = PF_LOCAL;
    addr.sun_path[0] = 0;
    memcpy(addr.sun_path + 1, SOCKET_NAME, socket_name_size); //Discarding terminating \0 char.

    // Connecting.
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr.sun_family) + 1 + socket_name_size)) {
        __android_log_print(ANDROID_LOG_ERROR, NDCRASH_LOG_TAG, "Couldn't connect socket, error: %s (%d)", strerror(errno), errno);
        close(sock);
        return;
    }

    // Sending.
    const int sent = send(sock, &msg, sizeof(msg), 0);
    if (sent < 0) {
        __android_log_print(ANDROID_LOG_ERROR, NDCRASH_LOG_TAG, "Send error: %s (%d)", strerror(errno), errno);
    } else if (sent != sizeof(msg)) {
        __android_log_print(ANDROID_LOG_ERROR, NDCRASH_LOG_TAG, "Error: couldn't send whole message, sent bytes: %d, message size: %d", sent, sizeof(msg));
    } else {
        __android_log_print(ANDROID_LOG_INFO, NDCRASH_LOG_TAG, "Successfuly sent data to crash service.");
    }

    // Blocking read.
    char c = 0;
    if (recv(sock, &c, 1, 0) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, NDCRASH_LOG_TAG, "Recv error: %s (%d)", strerror(errno), errno);
    }

    close(sock);
}

enum ndcrash_error ndcrash_out_init() {
    if (ndcrash_out_context_instance) {
        return ndcrash_error_already_initialized;
    }

    // Initializing context instance.
    ndcrash_out_context_instance = (struct ndcrash_out_context *) malloc(sizeof(struct ndcrash_out_context));
    memset(ndcrash_out_context_instance, 0, sizeof(struct ndcrash_out_context));

    // Trying to register signal handler.
    if (!ndcrash_register_signal_handler(&ndcrash_out_signal_handler, ndcrash_out_context_instance->old_handlers)) {
        ndcrash_in_deinit();
        return ndcrash_error_signal;
    }

    return ndcrash_ok;
}

void ndcrash_out_deinit() {
    if (!ndcrash_out_context_instance) return;
    ndcrash_unregister_signal_handler(ndcrash_out_context_instance->old_handlers);
    free(ndcrash_out_context_instance);
    ndcrash_out_context_instance = NULL;
}

#endif //ENABLE_OUTOFPROCESS