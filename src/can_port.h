#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/can.h>
#include <linux/can/raw.h>

struct can_port {
    // CAN file handle
    int fd;

    //write buffer
    char *write_buffer;
    //^const?
    int write_buffer_size;
    int write_buffer_offset;

    //read buffer stuff
    struct can_frame *read_buffer;
    int awaiting_read;
};

int can_open(struct can_port *port, char *interface_name);

int can_is_open(struct can_port *port);

int can_init(struct can_port **pport);

int can_close(struct can_port *port);

int can_write(struct can_port *can_port, struct can_frame *can_frame);

int can_read(struct can_port *can_port, struct can_frame *can_frame);

int can_notify_read(struct can_port *can_port);
