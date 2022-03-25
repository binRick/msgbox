#include "msgbox.h"

#include "log/log.c"
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define true     1
#define false    0

static int      done            = false;
static msg_Conn *listening_conn = NULL;
bool            do_listen       = true;

static char     *event_names[] = {
  "msg_message",
  "msg_request",
  "msg_reply",
  "msg_listening",
  "msg_listening_ended",
  "msg_connection_ready",
  "msg_connection_closed",
  "msg_connection_lost",
  "msg_error"
};


void update(msg_Conn *conn, msg_Event event, msg_Data data) {
  log_trace("Server: received event %s.", event_names[event]);

  if (event == msg_error) {
    log_error("Server: error: %s.", msg_as_str(data));
  }

  if (event == msg_listening) {
    log_info("Listening.......");
    listening_conn = conn;
  }

  if (event == msg_message || event == msg_request) {
    if (event == msg_request) {
      log_debug(
        AC_RESETALL AC_GREEN AC_REVERSED "Server: msg request is '%s'." AC_RESETALL
        "",
        msg_as_str(data)
        );
    }else{
      log_trace(
        AC_RESETALL AC_YELLOW AC_REVERSED "Server: message is '%s'." AC_RESETALL
        "",
        msg_as_str(data)
        );
    }
    msg_Data out_data = msg_new_data_space(data.num_bytes + strlen("echo:"));
    sprintf(out_data.bytes, "echo:%s", msg_as_str(data));
    msg_send(conn, out_data);
    msg_delete_data(out_data);
  }

  if (event == msg_connection_closed) {
    log_info("<%d> CONNECTION CLOSED", getpid());
  }
  if (event == msg_connection_closed) done = true;
}


int main(int argc, char **argv) {
  log_set_level(LOG_DEBUG);
  if (argc != 2 || (strcmp(argv[1], "udp") && strcmp(argv[1], "tcp"))) {
    char *name = basename(argv[0]);
    printf("\n  Usage: %s (tcp|udp)", name);
    return(2);
  }

  char *protocol = argv[1];
  int  port      = protocol[0] == 't' ? 2345 : 2468;

  char address[128];

  snprintf(address, 128, "%s://*:%d", protocol, port);
  log_info("Server: listening at address %s", address);
  msg_listen(address, update);

  int timeout_in_ms = 10;

  while (!done) {
    msg_runloop(timeout_in_ms);
  }
  msg_unlisten(listening_conn);

  // Give the runloop a chance to see the msg_listening_ended event.
  msg_runloop(timeout_in_ms);

  return(0);
}
