#include "msgbox.h"

#include "CArray.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

// Possible future features
//
// * By default, all data objects sent to callbacks are dynamic and owned by us.
//   We free them when the callback returns. Add a function to make it possible for
//   a callback to retain ownership of a data buffer so that buffer doesn't have to
//   be copied by the user. This doesn't have to be in v1.
//

// Make it possible to turn on and off the assert macro based
// on the DEBUG preprocessor definition.

// TODO TEMP
#define DEBUG

#ifdef DEBUG
#include <assert.h>
#else
#define assert(x)
#endif

#define true 1
#define false 0

// This is a possible return value for functions that return
// an error string when an error is encountered.
#define no_error NULL

// We only need 6 bytes, but it's nice to keep the user's data
// pointer 8-byte aligned, so we reserve 8 bytes for the header.
#define header_len 8
#define recv_buffer_len 32768

#define sock_in_size sizeof(struct sockaddr_in)

// TEMP TODO remove; these are for temporary testing
#include <stdio.h>

typedef struct {
  msg_Conn *conn;
  msg_Event event;
  msg_Data data;
  void *free_after_call;
} PendingCallback;

// TODO after tcp is added and most functionality is done
// Solidify the rules for how we call free after a callback is called.
// Right now they're something like this:
// * free(free_after_call) if free_after_call != NULL
// * free data->bytes if it's a tcp message and !NULL; (? revisit)
// * free data->bytes - header_len if it's a udp message and !NULL.
//
// For connection closes (closed or lost), the msg_Conn object will have already
// been removed from conns (and the socket removed from poll_fds), and the conn
// object will be a copy of the original set as free_after_call.

static int init_done = false;
static CArray immediate_callbacks = NULL;

// These arrays have corresponding elements at the same index.
static CArray poll_fds = NULL;
static CArray conns = NULL;

typedef struct {
  uint16_t reply_id;
  uint16_t num_packets;
  uint16_t packet_id;
} Header;
// TODO
// * Make sure this struct is used throughout the code.
// * Do I need to worry about byte-order? I suspect yes.

// Header values for the reply_id field.
static uint16_t next_reply_id = 1;
static const uint16_t one_way_msg = 1;
static const uint16_t is_reply_mask = 1 << 15;
static const uint16_t max_reply_id = (1 << 15) - 1;  // = bits:01..1 with 15 ones.

///////////////////////////////////////////////////////////////////////////////
//  Internal functions.

void CArrayRemoveLast(CArray array) {
  CArrayRemoveElement(array, CArrayElement(array, array->count - 1));
}

void remove_last_polling_conn() {
  CArrayRemoveLast(conns);
  CArrayRemoveLast(poll_fds);
}

static void init_if_needed() {
  if (init_done) return;

  immediate_callbacks = CArrayNew(16, sizeof(PendingCallback));
  poll_fds = CArrayNew(8, sizeof(struct pollfd));
  conns = CArrayNew(8, sizeof(msg_Conn *));

  init_done = true;
}

static void send_callback(msg_Conn *conn, msg_Event event, msg_Data data) {
  //printf("%s: event=%d.\n", __func__, event);
  PendingCallback pending_callback = {conn, event, data, NULL};
  CArrayAddElement(immediate_callbacks, pending_callback);
}

static void send_callback_error(msg_Conn *conn, const char *msg, void *to_free) {
  //printf("%s: %s.\n", __func__, msg);
  PendingCallback pending_callback = {conn, msg_error, msg_new_data(msg), to_free};
  CArrayAddElement(immediate_callbacks, pending_callback);
}

static void send_callback_os_error(msg_Conn *conn, const char *msg, void *to_free) {
  //printf("%s: %s.\n", __func__, msg);
  static char err_msg[1024];
  snprintf(err_msg, 1024, "%s: %s", msg, strerror(errno));
  send_callback_error(conn, err_msg, to_free);
}

// TODO Use this function as much as possible.
// Returns no_error (NULL) on success; otherwise an error string.
static char *set_sockaddr_for_conn(struct sockaddr_in *sockaddr, msg_Conn *conn) {
  memset(sockaddr, 0, sizeof(struct sockaddr_in));
  sockaddr->sin_family = AF_INET;
  sockaddr->sin_port = htons(conn->remote_port);
  char *ip_str = conn->remote_address;
  if (strcmp(ip_str, "*") == 0) {
    sockaddr->sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (inet_aton(ip_str, &sockaddr->sin_addr) == 0) {
    static char err_msg[1024];
    snprintf(err_msg, 1024, "Couldn't parse ip string '%s'.", ip_str);
    return err_msg;
  }
  return no_error;
}

// Returns no_error (NULL) on success, and sets the protocol_type,
// remote_address, and remote_port of the given conn.
// Returns an error string if there was an error.
static const char *parse_address_str(const char *address, msg_Conn *conn) {
  assert(conn != NULL);
  static char err_msg[1024];

  // TODO once v1 functionality is done, see if I can
  // encapsulate the error pattern into a one-liner; eg with a macro.

  // Parse the protocol type; either tcp or udp.
  const char *tcp_prefix = "tcp://";
  const char *udp_prefix = "udp://";
  size_t prefix_len = 6;
  if (strncmp(address, tcp_prefix, prefix_len) == 0) {
    conn->protocol_type = SOCK_STREAM;
  } else if (strncmp(address, udp_prefix, prefix_len) == 0) {
    conn->protocol_type = SOCK_DGRAM;
  } else {
    // address has no recognizable prefix.
    snprintf(err_msg, 1024, "Failing due to unrecognized prefix: %s", address);
    return err_msg;
  }

  // Parse the ip_str.
  // The syntax within ip_str is checked by inet_aton for us.
  const char *ip_start = address + prefix_len;
  char *colon = strchr(ip_start, ':');
  if (colon == NULL) {
    snprintf(err_msg, 1024, "Can't parse address '%s'; missing colon after ip", address);
    return err_msg;
  }
  size_t ip_len = colon - ip_start;
  if (ip_len > 15 || ip_len < 1) {
    snprintf(err_msg, 1024,
        "Failing because ip length=%zd; expected to be 1-15 (in addresss '%s')",
        ip_len, address);
    return err_msg;
  }
  char *ip_str_end = stpncpy(conn->remote_address, ip_start, ip_len);
  *ip_str_end = '\0';

  // Parse the port.
  int base_ten = 10;
  char *end_ptr = NULL;
  if (*(colon + 1) == '\0') {
    snprintf(err_msg, 1024, "Empty port string in address '%s'", address);
    return err_msg;
  }
  conn->remote_port = (int)strtol(colon + 1, &end_ptr, base_ten);
  if (*end_ptr != '\0') {
    snprintf(err_msg, 1024, "Invalid port string in address '%s'", address);
    return err_msg;
  }
  
  return no_error;
}

// Reads the header of a udp packet.
// Returns true on success; false on failure.
static int read_header(int sock, msg_Conn *conn, int *num_packets, int *packet_id) {
  static uint16_t buffer[(header_len / sizeof(uint16_t))];
  ssize_t bytes_recvd = recv(sock, buffer, header_len, MSG_PEEK);
  if (bytes_recvd == -1) {
    send_callback_os_error(conn, "recv", NULL);
    return false;
  }
  conn->reply_id = ntohs(buffer[0]);
  *num_packets = ntohs(buffer[1]);
  *packet_id = ntohs(buffer[2]);
  return true;
}

static void read_from_socket(int sock, msg_Conn *conn) {
  //printf("%s(%d, %p)\n", __func__, sock, conn);
  int num_packets, packet_id;
  if (!read_header(sock, conn, &num_packets, &packet_id)) return;
  //printf("reply_id(raw)=%d num_packets=%d packet_id=%d.\n", conn->reply_id, num_packets, packet_id);

  int is_reply = !!(conn->reply_id & is_reply_mask);
  conn->reply_id &= !is_reply_mask;  // Ensure the is_reply bit is off.

  //printf("reply_id=%d is_reply=%d.\n", conn->reply_id, is_reply);

  msg_Event event = msg_message;
  if (conn->reply_id) event = is_reply ? msg_reply : msg_request;

  if (num_packets == 1) {
    char *buffer = malloc(recv_buffer_len);
    int default_options = 0;
    struct sockaddr_in remote_sockaddr;
    socklen_t remote_sockaddr_size = sizeof(remote_sockaddr);
    ssize_t bytes_recvd = recvfrom(sock, buffer, recv_buffer_len, default_options,
        (struct sockaddr *)&remote_sockaddr, &remote_sockaddr_size);

    if (bytes_recvd == -1) return send_callback_os_error(conn, "recvfrom", NULL);

    msg_Data data = {bytes_recvd - header_len, buffer + header_len};
    strcpy(conn->remote_address, inet_ntoa(remote_sockaddr.sin_addr));
    conn->remote_port = ntohs(remote_sockaddr.sin_port);

    send_callback(conn, event, data);

  } else {
    // TODO Handle the multi-packet case.
  }
}

static msg_Conn *new_connection(void *conn_context, msg_Callback callback) {
  msg_Conn *conn = (msg_Conn *)malloc(sizeof(msg_Conn));
  memset(conn, 0, sizeof(msg_Conn));
  conn->conn_context = conn_context;
  conn->callback = callback;
  return conn;
}

// Sets up sockaddr based on address. If an error occurs, the error callback
// is scheduled.  Returns true on success.
// conn and its polling fd are added the polling conns arrays on success.
static int setup_sockaddr(struct sockaddr_in *sockaddr, const char *address, msg_Conn *conn) {
  int use_default_protocol = 0;
  int sock = socket(AF_INET, SOCK_DGRAM, use_default_protocol);
  if (sock == -1) {
    send_callback_os_error(conn, "socket", conn);
    return false;
  }

  // We have a real socket, so add entries to both poll_fds and conns.
  conn->socket = sock;
  CArrayAddElement(conns, conn);

  struct pollfd *poll_fd = (struct pollfd *)CArrayNewElement(poll_fds);
  poll_fd->fd = sock;
  poll_fd->events = POLLIN;

  const char *err_msg = parse_address_str(address, conn);
  if (err_msg != no_error) {
    remove_last_polling_conn();
    send_callback_error(conn, err_msg, conn);
    return false;
  }

  // Set up the sockaddr_in struct.
  memset(sockaddr, 0, sock_in_size);
  sockaddr->sin_family = AF_INET;
  sockaddr->sin_port = htons(conn->remote_port);
  if (strcmp(conn->remote_address, "*") == 0) {
    sockaddr->sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (inet_aton(conn->remote_address, &sockaddr->sin_addr) == 0) {
    remove_last_polling_conn();
    static char err_msg[1024];
    snprintf(err_msg, 1024, "Couldn't parse ip string '%s'.", conn->remote_address);
    send_callback_error(conn, err_msg, conn);
    return false;
  }

  return true;
}

typedef int (*SocketOpener)(int, const struct sockaddr *, socklen_t);

static void open_socket(const char *address, void *conn_context,
    msg_Callback callback, int for_listening) {
  init_if_needed();

  msg_Conn *conn = new_connection(conn_context, callback);
  conn->for_listening = for_listening;
  struct sockaddr_in *sockaddr = alloca(sock_in_size);
  if (!setup_sockaddr(sockaddr, address, conn)) return;  // There was an error.

  SocketOpener sys_open_sock = for_listening ? bind : connect;
  int ret_val = sys_open_sock(conn->socket, (struct sockaddr *)sockaddr, sock_in_size);
  if (ret_val == -1) {
    send_callback_os_error(conn, "bind", conn);
    return remove_last_polling_conn();
  }

  msg_Event event = for_listening ? msg_listening : msg_connection_ready;
  send_callback(conn, event, msg_no_data);
}


///////////////////////////////////////////////////////////////////////////////
//  Public functions.

void msg_runloop(int timeout_in_ms) {
  nfds_t num_fds = poll_fds->count;

  // TEMP
  static int num_prints = 0;
  if (num_prints < 3) {
    //printf("About to call poll.\n");
    //printf("num_fds=%d\n", num_fds);
    if (num_fds > 0) {
      struct pollfd *pfd = (struct pollfd *)poll_fds->elements;
      //printf("first fd=%d\n", pfd->fd);
    }
    num_prints++;
  }

  int ret = poll((struct pollfd *)poll_fds->elements, num_fds, timeout_in_ms);

  if (ret == -1) {
    // It's difficult to send a standard error callback to the user here because
    // we don't know which connection (and therefore which callback pointer) to use;
    // also, critical errors should only happen here due to bugs in msgbox itself.
    if (errno == EFAULT || errno == EINVAL) {
      // These theoretically can only be my fault; still, let the user know.
      fprintf(stderr, "Internal msgbox error during 'poll' call: %s\n", strerror(errno));
    }
    // Otherwise errno is EAGAIN or EINTR, both non-critical.
  } else if (ret > 0) {
    CArrayFor(struct pollfd *, poll_fd, poll_fds) {
      if (!(poll_fd->revents & POLLIN)) continue;
      int index = CArrayIndexOf(poll_fds, poll_fd);
      msg_Conn *conn = CArrayElementOfType(conns, index, msg_Conn *);
      read_from_socket(poll_fd->fd, conn);
    }
  }

  // Save the state of pending callbacks so that users can add new callbacks
  // from within their callbacks.
  CArray saved_immediate_callbacks = immediate_callbacks;
  immediate_callbacks = CArrayNew(16, sizeof(PendingCallback));

  CArrayFor(PendingCallback *, call, saved_immediate_callbacks) {
    call->conn->callback(call->conn, call->event, call->data);
    if (call->free_after_call) free(call->free_after_call);
  }

  // TODO free data from called callbacks

  // TODO handle any timed callbacks

  CArrayDelete(saved_immediate_callbacks);
}

void msg_listen(const char *address, void *conn_context, msg_Callback callback) {
  int for_listening = true;
  open_socket(address, conn_context, callback, for_listening);
}

void msg_connect(const char *address, void *conn_context, msg_Callback callback) {
  int for_listening = false;
  open_socket(address, conn_context, callback, for_listening);
}

void msg_disconnect(msg_Conn *conn) {
}

void msg_send(msg_Conn *conn, msg_Data data) {
  //printf("%s: '%s'\n", __func__, msg_as_str(data));

  // Set up the header.
  // TODO Encapsulate header setup in a function.
  Header *header = (Header *)(data.bytes - header_len);
  header->reply_id = htons(one_way_msg);
  // TODO Be able to handle multi-packet data.
  header->num_packets = htons(1);
  // packet_id is ignored for one-packet messages.

  int default_options = 0;
  //printf("protocol_type=%d for_listening=%d.\n", conn->protocol_type, conn->for_listening);
  if (conn->protocol_type == msg_udp && conn->for_listening) {
    //printf("Using sendto.\n");
    struct sockaddr_in sockaddr;
    char *err_msg = set_sockaddr_for_conn(&sockaddr, conn);
    if (err_msg) return send_callback_error(conn, err_msg, NULL);
    sendto(conn->socket, data.bytes - header_len, data.num_bytes + header_len, default_options,
        (struct sockaddr *)&sockaddr, sizeof(struct sockaddr_in));
  } else {
    //printf("Using send.\n");
    send(conn->socket, data.bytes - header_len, data.num_bytes + header_len, default_options);
  }
}

void msg_get(msg_Conn *conn, msg_Data data, void *reply_context) {
}

char *msg_as_str(msg_Data data) {
  return data.bytes;
}

msg_Data msg_new_data(const char *str) {
  // Allocate room for the string with +1 for the null terminator.
  msg_Data data = msg_new_data_space(strlen(str) + 1);
  strcpy(data.bytes, str);
  return data;
}

msg_Data msg_new_data_space(size_t num_bytes) {
  msg_Data data = {num_bytes, malloc(num_bytes + header_len)};
  data.bytes += header_len;
  return data;
}

void msg_delete_data(msg_Data data) {
  free(data.bytes - header_len);
}

char *msg_error_str(msg_Data data) {
  return msg_as_str(data);
}

void *msg_no_context = NULL;

msg_Data msg_no_data = {0, NULL};

const int msg_tcp = SOCK_STREAM;
const int msg_udp = SOCK_DGRAM;

