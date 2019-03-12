#include "can_port.h"
#include "util.h"
#include "erlcmd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <time.h>
#include <poll.h>

#include <sys/socket.h>
#include <net/if.h>

int can_init(struct can_port **pport)
{
    struct can_port *port = malloc(sizeof(struct can_port));
    *pport = port;

    port->fd = -1;

    //write buffer stuff
    port->write_buffer_size = 0;
    port->write_buffer = NULL;

    //read buffer stuff
    port->read_buffer = NULL;

    //init as not CAN FD
    port->is_canfd = false;

    return 0;
}

int can_is_open(struct can_port *port)
{
    return port->fd != -1;
}

int can_close(struct can_port *port)
{
  close(port->fd);
  port->fd = -1;
  return 0;
}

int can_open(struct can_port *can_port, char *interface_name, char *interface_type, long *rcvbuf_size, long *sndbuf_size)
{
  int s;
  int canfd_on;
  struct sockaddr_can addr;
  struct ifreq ifr;


  //open socket
  if ((s = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0)
    return s;

  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, flags | O_NONBLOCK);

  can_port->fd = s;

  //get interface index
  strcpy(ifr.ifr_name, interface_name);
  ioctl(s, SIOCGIFINDEX, &ifr);

  //get interface type
  can_port->is_canfd = (0 == strcmp("canfd", interface_type));

  //add busoff error filter
  can_err_mask_t err_mask = CAN_ERR_MASK;
  setsockopt(s, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof(err_mask));

  /* try to switch the bridge socket into CAN FD mode */
  canfd_on = (int)can_port->is_canfd;
  setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &canfd_on, sizeof(canfd_on));

  //set buffersizes
  if(setsockopt(s, SOL_SOCKET, SO_RCVBUF, rcvbuf_size, sizeof(*rcvbuf_size)) < 0)
    errx(EXIT_FAILURE, "badrcvbuf");
  if(setsockopt(s, SOL_SOCKET, SO_SNDBUF, sndbuf_size, sizeof(*sndbuf_size)) < 0)
    errx(EXIT_FAILURE, "badsndbuf");

  //bind
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  return bind(s, (struct sockaddr *)&addr, sizeof(addr));
}

int can_write(struct can_port *can_port, struct can_frame *can_frame)
{
  return write(can_port->fd, can_frame, sizeof(struct can_frame));
}

int canfd_write(struct can_port *can_port, struct canfd_frame *canfd_frame)
{
  return write(can_port->fd, canfd_frame, sizeof(struct canfd_frame));
}

void encode_can_frame(char *resp, int *resp_index, struct can_frame *can_frame)
{
  ei_encode_list_header(resp, resp_index, 1);
  ei_encode_tuple_header(resp, resp_index, 2);
  ei_encode_ulong(resp, resp_index, (unsigned long) can_frame->can_id);

  ei_encode_binary(resp, resp_index, can_frame->data, can_frame->can_dlc);
}

void encode_canfd_frame(char *resp, int *resp_index, struct canfd_frame *canfd_frame)
{
  ei_encode_list_header(resp, resp_index, 1);
  ei_encode_tuple_header(resp, resp_index, 2);
  ei_encode_ulong(resp, resp_index, (unsigned long) canfd_frame->can_id);

  ei_encode_binary(resp, resp_index, canfd_frame->data, canfd_frame->len);
}

int can_read(struct can_port *can_port, struct can_frame *can_frame)
{
  return read(can_port->fd, can_frame, sizeof(struct can_frame));
}

int canfd_read(struct can_port *can_port, struct canfd_frame *canfd_frame)
{
  return read(can_port->fd, canfd_frame, sizeof(struct canfd_frame));
}

int can_read_into_buffer(struct can_port *can_port, int *resp_index)
{
  int num_read;
  struct can_frame can_frame;
  struct canfd_frame canfd_frame;

  bool is_canfd = can_port->is_canfd;

  for(num_read = 0; num_read < 1000; num_read++)
  {
    int res;

    if (is_canfd)
    {
      res = read(can_port->fd, &canfd_frame, sizeof(struct canfd_frame));
    }
    else
    {
      res = read(can_port->fd, &can_frame, sizeof(struct can_frame));
    }


    if(res <= 0)
    {
      //I think ENETDOWN is ok because catching netdown at a higher level?
      return (errno == EAGAIN || errno == ENETDOWN)? num_read : -1;
    }
    else
    {
      if (is_canfd)
      {
        encode_canfd_frame(can_port->read_buffer, resp_index, &canfd_frame);
      }
      else
      {
        encode_can_frame(can_port->read_buffer, resp_index, &can_frame);
      }
    }
  }
  return num_read;
}
