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

static canid_t parse_can_id(char* ptr)
{
    /* uint8_t* ptr = (uint8_t*) ptr; */
    canid_t can_id = (ptr[0]<<24) | (ptr[1]<<16) | (ptr[2]<<8) | (ptr[3]<<0);
    return can_id;
}

//request is an array of maps with keys {:id, :data, :data_size}
static void handle_write(const char *req, int *req_index)
{
    int num_tuple_elements;
    if(ei_decode_tuple_header(req, req_index, &num_tuple_elements) < 0 || num_tuple_elements != 2)
      errx(EXIT_FAILURE, "expecting frame with format {frame_id, frame_id}");
    char id[4]; 
    long id_len;
    if (ei_decode_binary(req, req_index, id, &id_len) < 0)
      errx(EXIT_FAILURE, "failed to extract frame id");
    long data_len;
    char data[8];
    if(ei_decode_binary(req, req_index, data, &data_len) < 0 || data_len > 8)
      errx(EXIT_FAILURE, "failed to extract frame data");

    struct can_frame can_frame;
    can_frame.can_id = parse_can_id(id);
    can_frame.can_dlc = data_len;
    memcpy(can_frame.data, data, data_len);

    int write_result = can_write(can_port, &can_frame);
    if(write_result >= 0) {
      send_ok_response();
    }
    else if(errno == EAGAIN) {
      send_error_response("buffer_full");
    } else {
      char buf[10];
      sprintf(buf, "enosend%d", write_result);
      errx(EXIT_FAILURE, buf);
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

static void handle_read(const char *req, int *req_index)
{
  struct can_frame can_frame;
  int bytes_read = can_read(can_port, &can_frame);
  if(bytes_read > 0 && (size_t) bytes_read >= sizeof(struct can_frame)) {
    //REVIEW: How did fhunleth determine the ammount of additional memory to alloc?
    //not really clear from :ei docs how large all the various headers are
    char *resp = malloc(32 + bytes_read);
    int resp_index = sizeof(uint16_t);
    resp[resp_index++] = response_id;
    ei_encode_version(resp, &resp_index);
    ei_encode_tuple_header(resp, &resp_index, 2);
    ei_encode_atom(resp, &resp_index, "ok");
    ei_encode_tuple_header(resp, &resp_index, 2);
    ei_encode_ulong(resp, &resp_index, (unsigned long) can_frame.can_id);
    //REVIEW: is it necessary to buffer this binary if it's under 8 bytes?
    ei_encode_binary(resp, &resp_index, can_frame.data, 8);
    erlcmd_send(resp, resp_index);
    free(resp);
  } else if(errno == EAGAIN) {
    send_error_response("nodata");
  } else if (bytes_read > 0) {
    send_error_response("partialframe");
  }
  else {
    errx(EXIT_FAILURE, "failed to read from can device");
  }
}

static struct request_handler request_handlers[] = {
  { "read", handle_read },
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
    char logfile[64];
    sprintf(logfile, "nerves_can-%d.log", 1);
    FILE *fp = fopen(logfile, "w+");
    log_location = fp;

    debug("Starting...");
#endif
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
