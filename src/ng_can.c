#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "erlcmd.h"
#include "util.h"

#include <poll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int main()
{
    uart_default_config(&current_config);
    if (uart_init(&uart,
                  handle_write_completed,
                  handle_read_completed,
                  handle_notify_read) < 0)
        errx(EXIT_FAILURE, "uart_init failed");

    struct erlcmd *handler = malloc(sizeof(struct erlcmd));
    erlcmd_init(handler, handle_elixir_request, NULL);

    for (;;) {
        struct pollfd fdset[3];

        fdset[0].fd = STDIN_FILENO;
        fdset[0].events = POLLIN;
        fdset[0].revents = 0;

        int timeout = -1; // Wait forever unless told by otherwise
        int count = uart_add_poll_events(uart, &fdset[1], &timeout);

        int rc = poll(fdset, count + 1, timeout);
        if (rc < 0) {
            // Retry if EINTR
            if (errno == EINTR)
                continue;

            err(EXIT_FAILURE, "poll");
        }

        if (fdset[0].revents & (POLLIN | POLLHUP)) {
            if (erlcmd_process(handler))
                break;
        }

        // Call uart_process if it added any events
        if (count)
            uart_process(uart, &fdset[1]);
    }

    // Exit due to Erlang trying to end the process.
    //
    if (uart_is_open(uart))
        uart_flush_all(uart);

    return 0;
}

