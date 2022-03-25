#define MSG_CONTENT    "xxx"
/*****************************/
#include "msgbox.h"
/*****************************/
#include "log/log.c"
#include "human/bytes.c"
#include <libgen.h>
#include <stdio.h>
#include <string.h>
/*****************************/
#define true     1
#define false    0
/*****************************/
static int  done           = false;
static int  connected      = false;
int timeout_in_ms = 10;
static char *event_names[] = {
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
#define REQUEST_CONTENT    "yyy"
#define MAX_RECVD_BYTES 1024  * 1
static int recv_bytes = 0;

/*****************************/
void update(msg_Conn *conn, msg_Event event, msg_Data data) {
  log_info("<%d> Client: received event %s. %s recvd|", getpid(), event_names[event],bytes_to_string(recv_bytes));
  if (event == msg_connection_ready) {
    connected = true;
    log_info("<%d> conn ready!", getpid());
    msg_Data data = msg_new_data(MSG_CONTENT);
    msg_send(conn, data);
    msg_delete_data(data);
  }

  if (event == msg_connection_closed) {
    connected = false;
  }
  if (event == msg_error) {
    log_error("Client: error: %s.", msg_as_str(data));
  }


  if (event == msg_reply) {
      recv_bytes += strlen(msg_as_str(data));
    log_info(
      AC_RED "Message Reply '%s'." AC_RESETALL
      " "
      AC_GREEN "reply_context: '%s'." AC_RESETALL
      "",
      msg_as_str(data),
      conn->reply_context ? (char *)conn->reply_context : "<null>"
      );
    msg_disconnect(conn);
    //done = true;
  }else if (event == msg_message) {
      recv_bytes += strlen(msg_as_str(data));
    log_info(
      AC_RESETALL AC_YELLOW "Client: message is '%s'." AC_RESETALL
      "",
      msg_as_str(data)
      );

    msg_Data data1 = msg_new_data(REQUEST_CONTENT);
    msg_get(conn, data1, REQUEST_CONTENT);
    msg_delete_data(data1);
  }
} /* update */


/*****************************/
int main(int argc, char **argv) {
  log_set_level(LOG_DEBUG);
  if (argc != 2 || (strcmp(argv[1], "udp") && strcmp(argv[1], "tcp"))) {
    char *name = basename(argv[0]);
    printf("\n  Usage: %s (tcp|udp)\n\nMeant to be run after echo_server is started.\n", name);
    return(2);
  }

  char *protocol = argv[1];
  int  port      = protocol[0] == 't' ? 49113 : 2468;

  char address[128];

  snprintf(address, 128, "%s://127.0.0.1:%d", protocol, port);
  log_trace("Client: connecting to address %s", address);
  while (!done && (recv_bytes < MAX_RECVD_BYTES)) {
    if (!connected) {
      msg_connect(address, update, msg_no_context);
    }
    msg_runloop(timeout_in_ms);
//    sleep(1);
  }
  log_info("exiting....");
  return(0);
}
