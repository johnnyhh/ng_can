#include "can_comm.h"

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

#include <linux/can.h>
#include <linux/can/raw.h>



int can_init(struct can_port **pport)
{
    struct can_port *port = malloc(sizeof(struct can_port));
    *pport = port;

    port->fd = -1;
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

int can_open(struct can_port *can_port, char *interface_name)
{
  int s;
  struct sockaddr_can addr;
  struct ifreq ifr;

  //open socket
  if ((s = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0)
    return ERL_DRV_ERROR_ERRNO;

  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, flags | O_NONBLOCK);

  can_port->fd = s;

  //get interface index
  strcpy(ifr.ifr_name, interface_name);
  ioctl(s, SIOCGIFINDEX, &ifr);

  //bind
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;

  return bind(s, (struct sockaddr *)&addr, sizeof(addr));
}
