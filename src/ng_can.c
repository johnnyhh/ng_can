#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "erlcmd.h"
#include "util.h"
#include "can_port.h"

#include <poll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define ERR_STR_MAX_LEN 64

struct request_handler {
  const char *name;
  void (*handler)(const char *req, int *req_index);
};

static struct can_port *can_port = NULL;

// Utilities
static const char response_id = 'r';
static const char error_id = 'e';
static const char notification_id = 'n';

/**
 * @brief Send :ok back to Elixir
 */
static void send_ok_response()
{
  char resp[256];
  int resp_index = sizeof(uint16_t); // Space for payload size
  resp[resp_index++] = response_id;
  ei_encode_version(resp, &resp_index);
  ei_encode_atom(resp, &resp_index, "ok");
  erlcmd_send(resp, resp_index);
}

/**
 * @brief Send a response of the form {:error, reason}
 *
 * @param reason a reason (sent back as an atom)
 */
static void send_error_notification(const char *reason)
{
  char resp[256];
  int resp_index = sizeof(uint16_t); // Space for payload size
  resp[resp_index++] = error_id;
  ei_encode_version(resp, &resp_index);
  ei_encode_tuple_header(resp, &resp_index, 2);
  ei_encode_atom(resp, &resp_index, "error");
  ei_encode_binary(resp, &resp_index, reason, strlen(reason));
  erlcmd_send(resp, resp_index);
}

static struct can_frame parse_can_frame(const char *req, int *req_index)
{
    struct can_frame can_frame = { 0 };

    int num_tuple_elements;
    if(ei_decode_tuple_header(req, req_index, &num_tuple_elements) < 0 || num_tuple_elements != 2)
      errx(EXIT_FAILURE, "Bad Tuple");

    unsigned long id;
    if(ei_decode_ulong(req, req_index, &id) < 0)
      errx(EXIT_FAILURE, "Bad Can ID");

    long data_len;
    if(ei_decode_binary(req, req_index, can_frame.data, &data_len) < 0 || data_len > CAN_MAX_DLEN)
      errx(EXIT_FAILURE, "Bad Data");

    can_frame.can_id = id;
    can_frame.can_dlc = data_len;

    return can_frame;
}

static struct canfd_frame parse_canfd_frame(const char *req, int *req_index)
{
    struct canfd_frame canfd_frame = { 0 };

    int num_tuple_elements;
    if(ei_decode_tuple_header(req, req_index, &num_tuple_elements) < 0 || num_tuple_elements != 2)
      errx(EXIT_FAILURE, "Bad Tuple");

    unsigned long id;
    if(ei_decode_ulong(req, req_index, &id) < 0)
      errx(EXIT_FAILURE, "Bad Can ID");

    long data_len;
    if(ei_decode_binary(req, req_index, canfd_frame.data, &data_len) < 0 || data_len > CANFD_MAX_DLEN)
      errx(EXIT_FAILURE, "Bad Data");

    canfd_frame.can_id = id;
    canfd_frame.len = data_len;

    return canfd_frame;
}

static int write_buffer(const char *req, int *req_index, int num_frames)
{
  bool is_canfd = can_port->is_canfd;

  for (int i = 0; i < num_frames; i++)
  {
    int this_frame_offset = *req_index;
    int write_result;

    if (is_canfd)
    {
      struct canfd_frame canfd_frame = parse_canfd_frame(req, req_index);
      write_result = canfd_write(can_port, &canfd_frame);
    }
    else
    {
      struct can_frame can_frame = parse_can_frame(req, req_index);
      write_result = can_write(can_port, &can_frame);
    }

    if(write_result < 0 && (errno == EAGAIN || errno == ENOBUFS))
    {
      //enqueue the remaining frames
      int num_unsent = num_frames - i;
      if (can_port->write_buffer_size != 0)
      {
        free(can_port->write_buffer);
        can_port->write_buffer = NULL;
      }

      can_port->write_buffer_size = num_unsent;
      int num_bytes = ENCODED_WRITE_FRAME_SIZE * num_unsent;
      char *buffer = malloc(num_bytes);
      memcpy(buffer, req + this_frame_offset, num_bytes);
      can_port->write_buffer = buffer;
      return -1;
    }
    else if(write_result < 0)
    {
      char err_str[ERR_STR_MAX_LEN] = { 0 };
      snprintf(err_str, ERR_STR_MAX_LEN, "write() error: %d", errno);
      send_error_notification(err_str);
      errx(EXIT_FAILURE, err_str);
    }
  }

  return 0;
}

//request is an array of maps with keys {:id, :data, :data_size}
static void handle_write(const char *req, int *req_index)
{
  int num_frames;

  if(ei_decode_list_header(req, req_index, &num_frames) < 0)
  {
    errx(EXIT_FAILURE, "Expecting a list of frames");
  }

  write_buffer(req, req_index, num_frames);
  send_ok_response();
}

static void process_write_buffer()
{
  int req_index = 0;

  if (can_port->write_buffer_size != 0)
  {
    if(write_buffer(can_port->write_buffer, &req_index, can_port->write_buffer_size) == 0)
    {
      free(can_port->write_buffer);
      can_port->write_buffer = NULL;
      can_port->write_buffer_size = 0;
    }
  }
}

static void handle_open(const char *req, int *req_index)
{
  int arity;
  if (ei_decode_tuple_header(req, req_index, &arity) < 0 || arity != 4) {
    errx(EXIT_FAILURE, "badopentuple");
  }

  char interface_name[64] = { 0 };
  long binary_len;
  if(ei_decode_binary(req, req_index, interface_name, &binary_len) < 0) {
    errx(EXIT_FAILURE, "enoent");
  }

  char interface_type[64] = { 0 };
  if(ei_decode_binary(req, req_index, interface_type, &binary_len) < 0) {
    errx(EXIT_FAILURE, "enoent");
  }

  long rcvbuf_size;
  if(ei_decode_long(req, req_index, &rcvbuf_size) < 0)
    errx(EXIT_FAILURE, "noreadbufsize");

  long sndbuf_size;
  if(ei_decode_long(req, req_index, &sndbuf_size) < 0)
    errx(EXIT_FAILURE, "nowritebufsize");

  if (can_is_open(can_port))
    can_close(can_port);

  if (can_open(can_port, interface_name, interface_type, &rcvbuf_size, &sndbuf_size) >= 0) {
    send_ok_response();
  } else {
    send_error_notification("error opening can port");
  }
}

static void notify_read()
{
  //each can frame is ENCODED_READ_FRAME_SIZE, add 32 (not exactly calculated) for headers + other stuff
  can_port->read_buffer = malloc(36 + (1000 * ENCODED_READ_FRAME_SIZE));
  int resp_index = sizeof(uint16_t);
  can_port->read_buffer[resp_index++] = notification_id;
  ei_encode_version(can_port->read_buffer, &resp_index);
  ei_encode_tuple_header(can_port->read_buffer, &resp_index, 3);
  ei_encode_atom(can_port->read_buffer, &resp_index, "notif");
  int num_read = can_read_into_buffer(can_port, &resp_index);
  if (num_read < 0){
    char err_str[ERR_STR_MAX_LEN];
    snprintf(err_str, ERR_STR_MAX_LEN, "read() error: %d", errno);
    send_error_notification(err_str);
    errx(EXIT_FAILURE, err_str);
  }
  ei_encode_empty_list(can_port->read_buffer, &resp_index);
  ei_encode_ulong(can_port->read_buffer, &resp_index, num_read);
  erlcmd_send(can_port->read_buffer, resp_index);
  free(can_port->read_buffer);
  can_port->read_buffer = NULL;
}

static struct request_handler request_handlers[] = {
  { "write", handle_write },
  { "open", handle_open },
  { NULL, NULL }
};

static void handle_elixir_request(const char *req, void *cookie)
{
  (void) cookie;

  // Commands are of the form {Command, Arguments}:
  // { atom(), term() }
  int req_index = sizeof(uint16_t);
  if (ei_decode_version(req, &req_index, NULL) < 0)
    errx(EXIT_FAILURE, "Message version issue?");

  int arity;
  if (ei_decode_tuple_header(req, &req_index, &arity) < 0 ||
      arity != 2)
    errx(EXIT_FAILURE, "expecting {cmd, args} tuple");

  char cmd[MAXATOMLEN];
  if (ei_decode_atom(req, &req_index, cmd) < 0)
    errx(EXIT_FAILURE, "expecting command atom");

  for (struct request_handler *rh = request_handlers; rh->name != NULL; rh++) {
    if (strcmp(cmd, rh->name) == 0) {
      rh->handler(req, &req_index);
      return;
    }
  }
  errx(EXIT_FAILURE, "unknown command: %s", cmd);
}

int main(int argc, char *argv[])
{
#ifdef DEBUG
    char logfile[164];
    snprintf(logfile, sizeof(logfile) / sizeof(logfile[0]), "ng_can-%d.log", (int) getpid());
    FILE *fp = fopen(logfile, "w+");
    log_location = fp;

    debug("Starting!");
#endif
  if (can_init(&can_port) < 0)
    errx(EXIT_FAILURE, "can_init failed");

  struct erlcmd *handler = malloc(sizeof(struct erlcmd));
  erlcmd_init(handler, handle_elixir_request, NULL);

  for (;;)
  {
    struct pollfd fdset[3];
    int num_listeners = 2;

    fdset[0].fd = STDIN_FILENO;
    fdset[0].events = POLLIN;
    fdset[0].revents = 0;

    fdset[1].fd = can_port->fd;
    fdset[1].events = POLLIN;
    fdset[1].revents = 0;

    if(can_port->write_buffer_size > 0)
    {
      fdset[1].events |= POLLOUT;
    }

    int rc = poll(fdset, num_listeners, -1);
    if (rc < 0) {
      // Retry if EINTR
      if (errno == EINTR)
        continue;

      errx(EXIT_FAILURE, "poll");
    }

    if (fdset[0].revents & (POLLIN | POLLHUP)) {
      if (erlcmd_process(handler))
        break;
    }
    //ready to work through write buffer
    if (fdset[1].revents & POLLOUT) {
      process_write_buffer();
    }

    if (fdset[1].revents & POLLIN) {
      notify_read();
    }
  }

  return 0;
}
