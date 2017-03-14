#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct can_port {
    // CAN file handle
    int fd;
};

int can_is_open(struct can_port *port);

int can_close(struct can_port *port);

int can_open(struct can_port *port, char *interface_name);
