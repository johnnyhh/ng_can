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

struct request_handler {
  const char *name;
  void (*handler)(const char *req, int *req_index);
};

static struct can_port *can_port = NULL;

// Utilities
static const char response_id = 'r';
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
static void send_error_response(const char *reason)
{
  char resp[256];
  int resp_index = sizeof(uint16_t); // Space for payload size
  resp[resp_index++] = response_id;
  ei_encode_version(resp, &resp_index);
  ei_encode_tuple_header(resp, &resp_index, 2);
  ei_encode_atom(resp, &resp_index, "error");
  ei_encode_atom(resp, &resp_index, reason);
  erlcmd_send(resp, resp_index);
}

static struct can_frame parse_can_frame(const char *req, int *req_index)
{
    struct can_frame can_frame;
    int num_tuple_elements;
    if(ei_decode_tuple_header(req, req_index, &num_tuple_elements) < 0 || num_tuple_elements != 2)
      send_error_response("badtuple");
    unsigned long id;
    if (ei_decode_ulong(req, req_index, &id) < 0)
      send_error_response("badcanid");
    long data_len;
    char data[8] = "";
    if(ei_decode_binary(req, req_index, data, &data_len) < 0 || data_len > 8)
      send_error_response("badcandata");

    can_frame.can_id = id;
    can_frame.can_dlc = data_len;
    memcpy(can_frame.data, data, 8);
    return can_frame;
}

static int write_buffer(const char *req, int *req_index, int num_frames)
{
  for (int i = 0; i < num_frames; i++) {
    int start_index = *req_index;
    struct can_frame can_frame = parse_can_frame(req, req_index);
    int write_result = can_write(can_port, &can_frame);

    if(write_result < 0 && errno == EAGAIN) {
      //enqueue the remaining frames
      int num_unsent = num_frames - i;
      can_port->write_buffer_offset = 0;
      can_port->write_buffer_size = num_unsent;
      int frame_size = *req_index - start_index;
      int num_bytes = frame_size * num_unsent;
      char *buffer = malloc(num_bytes);
      memcpy(buffer, req + start_index, num_bytes);
      free(can_port->write_buffer);
      can_port->write_buffer = buffer;
      return -1;
    } else if(write_result < 0) {
      send_error_response("badwriteres");
    }
  }
  return 0;
}

//request is an array of maps with keys {:id, :data, :data_size}
static void handle_write(const char *req, int *req_index)
{
  int num_frames;
  if(ei_decode_list_header(req, req_index, &num_frames) < 0)
    send_error_response("expectinglist");
  write_buffer(req, req_index, num_frames);
  send_ok_response();
}

static void process_write_buffer()
{
  if(write_buffer(can_port->write_buffer, &(can_port->write_buffer_offset), can_port->write_buffer_size) == 0){
    free(can_port->write_buffer);
    can_port->write_buffer_size = 0;
    can_port->write_buffer_offset = 0;
  }
}

static void handle_open(const char *req, int *req_index)
{
  char interface_name[64];
  long binary_len;
  if(ei_decode_binary(req, req_index, interface_name, &binary_len) < 0) {
    send_error_response("enoent");
    return;
  }

  //REVIEW: is this necessary?
  interface_name[binary_len] = '\0';

  if (can_is_open(can_port))
    can_close(can_port);

  if (can_open(can_port, interface_name) >= 0) {
    send_ok_response();
  } else {
    send_error_response("error opening can port");
  }
}

//TODO:convert to list
static void handle_read(const char *req, int *req_index)
{
  struct can_frame can_frame;
  int bytes_read = can_read(can_port, &can_frame);
  if(bytes_read > 0 && (size_t) bytes_read >= sizeof(struct can_frame)) {
    char *resp = malloc(32 + bytes_read);
    int resp_index = sizeof(uint16_t);
    resp[resp_index++] = response_id;
    ei_encode_version(resp, &resp_index);
    ei_encode_tuple_header(resp, &resp_index, 2);
    ei_encode_atom(resp, &resp_index, "ok");
    encode_can_frame(resp, &resp_index, &can_frame);
    erlcmd_send(resp, resp_index);
    free(resp);
  } else if (bytes_read > 0) {
    send_error_response("partialframe");
  } else if(errno == EAGAIN) {
    send_error_response("nodata");
  }
  else {
    errx(EXIT_FAILURE, "failed to read from can device");
  }
}

static void handle_await_read(const char *req, int *req_index)
{
  if(can_port->awaiting_read == 1){
    send_error_response("busy");
  }
  else {
    can_port->awaiting_read = 1;
    send_ok_response();
  }
}

static void notify_read()
{
  //each can frame is ENCODED_FRAME_SIZE, add 32 (not exactly calculated) for headers + other stuff
  can_port->read_buffer = malloc(32 + (MAX_READBUF * ENCODED_FRAME_SIZE));
  int resp_index = sizeof(uint16_t);
  can_port->read_buffer[resp_index++] = notification_id;
  ei_encode_version(can_port->read_buffer, &resp_index);
  ei_encode_tuple_header(can_port->read_buffer, &resp_index, 2);
  ei_encode_atom(can_port->read_buffer, &resp_index, "notif");
  can_read_into_buffer(can_port, &resp_index);
  ei_encode_empty_list(can_port->read_buffer, &resp_index);
  erlcmd_send(can_port->read_buffer, resp_index);
  free(can_port->read_buffer);
  can_port->awaiting_read = 0;
}

static struct request_handler request_handlers[] = {
  { "await_read", handle_await_read },
  { "write", handle_write },
  { "read", handle_read },
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
    char logfile[64];
    /* sprintf(logfile, "ng_can-%d.log", (int) getpid()); */
    sprintf(logfile, "/root/logs/ng_can.log", (int) getpid());
    FILE *fp = fopen(logfile, "w+");
    log_location = fp;

    debug("Starting!");
#endif
  if (can_init(&can_port) < 0)
    errx(EXIT_FAILURE, "can_init failed");

  struct erlcmd *handler = malloc(sizeof(struct erlcmd));
  erlcmd_init(handler, handle_elixir_request, NULL);

  for (;;) {
    struct pollfd fdset[3];
    int num_listeners = 1;

    fdset[0].fd = STDIN_FILENO;
    fdset[0].events = POLLIN;
    fdset[0].revents = 0;

    fdset[1].fd = can_port->fd;
    fdset[1].revents = 0;

    if(can_port->write_buffer_size > 0) {
      num_listeners = 2;
      fdset[1].events = POLLOUT;
    }

    if(can_port->awaiting_read == 1) {
      num_listeners = 2;
      fdset[1].events |= POLLIN;
    }

    int rc = poll(fdset, num_listeners, -1);
    if (rc < 0) {
      // Retry if EINTR
      if (errno == EINTR)
        continue;

      debug("exiting due to error");
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
