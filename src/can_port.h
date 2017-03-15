#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//about a second of writes at 125k
#define MAX_WRITE_BUFFER 1000

#include <linux/can.h>
#include <linux/can/raw.h>

struct can_port {
    // CAN file handle
    int fd;

    //Fields to handle write logic
    int write_index;
    int write_buffer_size;
    struct can_frame write_buffer[MAX_WRITE_BUFFER];
};

int can_open(struct can_port *port, char *interface_name);

int can_is_open(struct can_port *port);

int can_init(struct can_port **pport);

int can_close(struct can_port *port);
