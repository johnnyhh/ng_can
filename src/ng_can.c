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

//request is an array of maps with keys {:id, :data, :data_size}
static void handle_write(const char *req, int *req_index)
{
  if(can_port->write_buffer_size > 0)
    send_error_response("buffer_full");

  //populate can_port write-related fields
  int num_frames;
  if(ei_decode_list_header(req, req_index, &num_frames) < 0)
    errx(EXIT_FAILURE, "Expecting a list of frames");

  if(num_frames > MAX_WRITE_BUFFER)
    send_error_response("too_many_frames");

  for( int i = 0; i < num_frames; i++ ) {
    int num_tuple_elements;
    if(ei_decode_tuple_header(req, req_index, &num_tuple_elements) < 0 || num_tuple_elements != 3)
      errx(EXIT_FAILURE, "expecting 3 elements in frame tuple");
    char id[32]; 
    long id_len;
    if (ei_decode_binary(req, req_index, id, &id_len) < 0)
      errx(EXIT_FAILURE, "failed to extract frame id");
    long data_len;
    char data[64];
    if(ei_decode_binary(req, req_index, data, &data_len) < 0)
      errx(EXIT_FAILURE, "failed to extract frame data");

    struct can_frame can_frame;
    can_frame.can_id = id;
    can_frame.can_dlc = data_len;
    can_frame.data = data;
    can_port->write_buffer[i] = can_frame;
  }
  send_ok_response();
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

  // If the uart was already open, close and open it again
  if (can_is_open(can_port))
    can_close(can_port);

  if (can_open(can_port, interface_name) >= 0) {
    send_ok_response();
  } else {
    send_error_response("error opening can port");
  }
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
  if (can_init(&can_port) < 0)
    errx(EXIT_FAILURE, "uart_init failed");

  struct erlcmd *handler = malloc(sizeof(struct erlcmd));
  erlcmd_init(handler, handle_elixir_request, NULL);

  for (;;) {
    struct pollfd fdset[3];

    fdset[0].fd = STDIN_FILENO;
    fdset[0].events = POLLIN;
    fdset[0].revents = 0;

    int rc = poll(fdset, 1, -1);
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
  }

  return 0;
}
