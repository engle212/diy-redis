#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <cassert>
#include <errno.h>
#include <vector>
#include <poll.h>
#include <fcntl.h>

const size_t k_max_msg = 4096;

struct Conn {
  int fd = -1;
  // application's intention, for the event loop
  bool want_read = false;
  bool want_write = false;
  bool want_close = false;
  // buffered input and output
  std::vector<uint8_t> incoming;
  std::vector<uint8_t> outgoing;
};

static void die(const char *message) {
  perror(message);
  exit(EXIT_FAILURE);
}

static void fd_set_nb(int fd) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static Conn *handle_accept(int fd) {
  // accept
  struct sockaddr_in client_addr = {};
  socklen_t socklen = sizeof(client_addr);
  int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
  if (connfd < 0) {
    return NULL;
  }
  // set the new connection fd to nonblocking mode
  fd_set_nb(connfd);
  // create a `struct Conn`
  Conn *conn = new Conn();
  conn->fd = connfd;
  conn->want_read = true;
  return conn;
}

static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
  buf.insert(buf.end(), data, data + len);
}

static void buf_consume(std::vector<uint8_t> &buf, size_t n) {
  buf.erase(buf.begin(), buf.begin() + n);
}

static bool try_one_request(Conn *conn) {
  // 3. Try to parse the accumulated buffer
  // Protocol: message header
  if (conn->incoming.size() < 4) {
    return false;
  }
  uint32_t len = 0;
  memcpy(&len, conn->incoming.data(), 4);
  if (len > k_max_msg) {
    conn->want_close = true;
    return false;
  }
  // Protocol: message body
  if (4 + len > conn->incoming.size()) {
    return false;
  }
  const uint8_t *request = &conn->incoming[4];
  // 4. Process the parsed message
  // ...
  // generate the response (echo)
  buf_append(conn->outgoing, (const uint8_t *)&len, 4);
  buf_append(conn->outgoing, request, len);
  // 5. Remove the message from `Conn::incoming`
  buf_consume(conn->incoming, 4+len);
  return true;
}

static void handle_read(Conn *conn) {
  // 1. Do a non-blocking read
  uint8_t buf[64 * 1024];
  ssize_t rv = read(conn->fd, buf, sizeof(buf));
  if (rv <= 0) {
    conn->want_close = true;
    return;
  }
  // 2. Add new data to the `Conn::incoming` buffer
  buf_append(conn->incoming, buf, (size_t)rv);
  // 3. Try to parse the accumulated buffer
  // 4. Process the parsed message
  // 5. Remove the message from `Conn::incoming`
  try_one_request(conn);

  // update the readiness intention
  if (conn->outgoing.size() > 0) { // has a response
    conn->want_read = false;
    conn->want_write = true;
  } else {
    conn->want_read = true;
    conn->want_write = false;
  }
}

static void handle_write(Conn *conn) {
  assert(conn->outgoing.size() > 0);
  ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());
  if (rv < 0) {
    conn->want_close = true;
    return;
  }
  // remove written data from `outgoing`
  buf_consume(conn->outgoing, (size_t)rv);

  if (conn->outgoing.size() == 0) { // all data written
    conn->want_read = true;
    conn->want_write = false;
  } else {
    conn->want_read = false;
    conn->want_write = true;
  }
}

int main() { 
  // Obtain a socket handle
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  // Set socket options
  int val = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
  // Bind to an address
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  addr.sin_addr.s_addr = htonl(0);
  int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
  if (rv) { die("bind()"); }
  // Listen
  rv = listen(fd, SOMAXCONN);
  if (rv) { die("listen()"); }

  std::vector<Conn *> fd2conn;

  std::vector<struct pollfd> poll_args;

  fd_set_nb(fd);

  while (true) {
    poll_args.clear();
    // put the listening sockets in the first position
    struct pollfd pfd = {fd, POLLIN, 0};
    poll_args.push_back(pfd);
    // the rest are connection sockets
    for (Conn *conn : fd2conn) {
      if (!conn) {
        continue;
      }
      struct pollfd pfd = {conn->fd, POLLERR, 0};
      // poll() flags from the application's intent
      if (conn->want_read) {
        pfd.events |= POLLIN;
      }
      if (conn->want_write) {
        pfd.events |= POLLOUT;
      }
      poll_args.push_back(pfd);
    }

    // wait for readiness
    int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
    if (rv < 0 && errno == EINTR) {
      continue; // not an error
    }
    if (rv < 0) {
      die("poll");
    }

    // handle the listening socket
    if (poll_args[0].revents) {
      if (Conn *conn = handle_accept(fd)) {
        // put it into the map
        if (fd2conn.size() <= (size_t)conn->fd) {
          fd2conn.resize(conn->fd + 1);
        }
        fd2conn[conn->fd] = conn;
      }
    }

    // handle connection sockets
    for (size_t i = 1; i < poll_args.size(); ++i) {
      uint32_t ready = poll_args[i].revents;
      Conn *conn = fd2conn[poll_args[i].fd];
      if (ready & POLLIN) {
        handle_read(conn); // application logic
      }
      if (ready & POLLOUT) {
        handle_write(conn); // application logic
      }

      if ((ready & POLLERR) || conn->want_close) {
        (void)close(conn->fd);
        fd2conn[conn->fd] = NULL;
        delete conn;
      }
    }

  }
}