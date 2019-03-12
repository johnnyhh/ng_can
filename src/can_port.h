#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/error.h>
#define MAX_READBUF 100
#define ENCODED_READ_FRAME_SIZE 27
#define ENCODED_WRITE_FRAME_SIZE 20

struct can_port {
    // CAN file handle
    int fd;

    //write buffer
    char *write_buffer;
    //^const?
    int write_buffer_size;

    //read buffer stuff
    char *read_buffer;

    //is CAN FD?
    bool is_canfd;
};

int can_open(struct can_port *port, char *interface_name, char *interface_type, long *rcvbuf_size, long *sndbuf_size);

int can_is_open(struct can_port *port);

int can_init(struct can_port **pport);

int can_close(struct can_port *port);

int can_write(struct can_port *can_port, struct can_frame *can_frame);

int canfd_write(struct can_port *can_port, struct canfd_frame *canfd_frame);

int can_read(struct can_port *can_port, struct can_frame *can_frame);

int canfd_read(struct can_port *can_port, struct canfd_frame *canfd_frame);

int can_read_into_buffer(struct can_port *can_port, int *resp_index);

void encode_can_frame(char *resp, int *resp_index, struct can_frame *can_frame);

void encode_canfd_frame(char *resp, int *resp_index, struct canfd_frame *canfd_frame);
