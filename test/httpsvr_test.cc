#include "ex_actor/api.h"

#include <gtest/gtest.h>

#include <sys/event.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <chrono>
#include <vector>
#include <optional>
#include <atomic>
#include <thread>
#include <sstream>

namespace ex = stdexec;

using namespace ex_actor::internal::logging;

class KqueueEventLoop {
 public:
  KqueueEventLoop() : kq_(-1), running_(false) {
    kq_ = kqueue();
    if (kq_ < 0) {
      Critical("Failed to create kqueue: {}", strerror(errno));
    }
  }

  ~KqueueEventLoop() { Stop(); }

  bool IsValid() const { return kq_ >= 0; }

  void Start() {
    if (running_.exchange(true)) return;
    worker_ = std::jthread([this]() { RunLoop(); });
  }

  void Stop() {
    running_ = false;
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void SetSocketNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

 private:
  void RunLoop() {
    ex_actor::internal::util::SetThreadName("kqueue_loop");
    while (running_) {
      struct kevent events[64];
      struct timespec ts {};
      ts.tv_sec = 0;
      ts.tv_nsec = 100000000;

      int n = kevent(kq_, nullptr, 0, events, 64, &ts);
      if (n < 0) {
        if (errno != EINTR) {
          Critical("kevent failed: {}", strerror(errno));
        }
        continue;
      }

      for (int i = 0; i < n; ++i) {
        auto* info = static_cast<IoInfo*>(events[i].udata);
        if (info) {
          info->triggered.store(true, std::memory_order_release);
          if (info->continuation) {
            info->continuation.resume();
          }
        }
      }
    }
  }

 public:
  struct IoInfo {
    std::atomic_bool triggered{false};
    std::coroutine_handle<> continuation;
  };

  void RegisterRead(int fd, IoInfo* info) {
    std::lock_guard lock(mutex_);
    struct kevent ev {};
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, info);
    if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
      Critical("Failed to register read event: {}", strerror(errno));
    }
  }

  void RegisterWrite(int fd, IoInfo* info) {
    std::lock_guard lock(mutex_);
    struct kevent ev {};
    EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, info);
    if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
      Critical("Failed to register write event: {}", strerror(errno));
    }
  }

  int Accept(int fd, struct sockaddr* addr, socklen_t* len) { return accept(fd, addr, len); }

  ssize_t Read(int fd, void* buf, size_t size) { return read(fd, buf, size); }

  ssize_t Write(int fd, const void* buf, size_t size) { return write(fd, buf, size); }

 private:
  int kq_;
  std::atomic_bool running_;
  std::jthread worker_;
  std::mutex mutex_;
};

struct HttpRequest {
  std::string method;
  std::string path;
  std::string version;
  std::string body;
};

struct HttpResponse {
  int status_code;
  std::string status_text;
  std::string body;

  std::string ToString() const {
    std::string response;
    response += "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Content-Type: text/plain\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += body;
    return response;
  }
};

class HttpHandler {
 public:
  HttpResponse HandleRequest(const HttpRequest& request) {
    if (request.method == "GET" && request.path == "/") {
      return {200, "OK", "Hello from ex-actor HTTP server!"};
    } else if (request.method == "GET" && request.path == "/ping") {
      return {200, "OK", "pong"};
    } else if (request.method == "GET" && request.path == "/health") {
      return {200, "OK", "healthy"};
    } else if (request.method == "POST" && request.path == "/echo") {
      return {200, "OK", request.body};
    }
    return {404, "Not Found", "404 - Page not found"};
  }
};

class ConnectionActor {
 public:
  explicit ConnectionActor(int client_fd, KqueueEventLoop* loop, HttpHandler* handler)
      : client_fd_(client_fd), loop_(loop), handler_(handler) {}

  exec::task<void> HandleRequest() {
    auto request_opt = co_await ReadRequest();
    if (!request_opt) {
      co_return;
    }
    auto response = handler_->HandleRequest(*request_opt);
    co_await WriteResponse(response);
    Info("Handled {} request for path: {}", request_opt->method, request_opt->path);
    co_return;
  }

 private:
  exec::task<std::optional<HttpRequest>> ReadRequest() {
    std::string buffer;
    while (true) {
      char chunk[1024];
      KqueueEventLoop::IoInfo info{};
      loop_->RegisterRead(client_fd_, &info);
      co_await std::suspend_always{};
      ssize_t n = loop_->Read(client_fd_, chunk, sizeof(chunk));
      if (n <= 0) {
        co_return std::nullopt;
      }
      buffer.append(chunk, n);
      if (buffer.find("\r\n\r\n") != std::string::npos) {
        break;
      }
    }

    size_t header_end = buffer.find("\r\n\r\n");
    std::string header = buffer.substr(0, header_end);
    std::string body = buffer.substr(header_end + 4);

    std::istringstream iss(header);
    HttpRequest request;
    iss >> request.method >> request.path >> request.version;
    request.body = body;

    co_return request;
  }

  exec::task<bool> WriteResponse(const HttpResponse& response) {
    std::string response_str = response.ToString();
    size_t total_written = 0;
    while (total_written < response_str.size()) {
      KqueueEventLoop::IoInfo info{};
      loop_->RegisterWrite(client_fd_, &info);
      co_await std::suspend_always{};
      ssize_t n = loop_->Write(client_fd_, response_str.data() + total_written,
                                response_str.size() - total_written);
      if (n <= 0) {
        co_return false;
      }
      total_written += n;
    }
    co_return true;
  }

  int client_fd_;
  KqueueEventLoop* loop_;
  HttpHandler* handler_;
};

class HttpServerActor {
 public:
  HttpServerActor(int port, KqueueEventLoop* loop, HttpHandler* handler)
      : loop_(loop), handler_(handler), server_fd_(-1), running_(true) {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
      Critical("Failed to create socket: {}", strerror(errno));
      return;
    }

    loop_->SetSocketNonBlocking(server_fd_);

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      Critical("Failed to bind: {}", strerror(errno));
      close(server_fd_);
      server_fd_ = -1;
      return;
    }

    if (listen(server_fd_, 128) < 0) {
      Critical("Failed to listen: {}", strerror(errno));
      close(server_fd_);
      server_fd_ = -1;
      return;
    }

    Info("HTTP server listening on port {}", port);
  }

  KqueueEventLoop* GetLoop() const { return loop_; }
  HttpHandler* GetHandler() const { return handler_; }

  void Stop() {
    running_ = false;
    if (server_fd_ >= 0) {
      close(server_fd_);
      server_fd_ = -1;
    }
  }

  bool IsRunning() const { return running_; }

  exec::task<void> Run() {
    while (running_) {
      KqueueEventLoop::IoInfo info{};
      loop_->RegisterRead(server_fd_, &info);
      co_await std::suspend_always{};

      if (!running_) break;

      struct sockaddr_in client_addr {};
      socklen_t client_len = sizeof(client_addr);
      int client_fd = loop_->Accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);

      if (client_fd < 0) {
        continue;
      }

      loop_->SetSocketNonBlocking(client_fd);
      Info("New connection accepted: {}", client_fd);

      auto conn_actor = co_await ex_actor::Spawn<ConnectionActor>(client_fd, loop_, handler_);
      co_await conn_actor.template Send<&ConnectionActor::HandleRequest>();
    }
  }

 private:
  KqueueEventLoop* loop_;
  HttpHandler* handler_;
  int server_fd_;
  std::atomic_bool running_;
};

struct TestResult {
  bool success;
  std::string message;
};

exec::task<TestResult> RunHttpServerTest() {
  TestResult result{false, ""};

  KqueueEventLoop loop;
  loop.Start();

  HttpHandler handler;
  ex_actor::Init(/*thread_pool_size=*/4);

  auto server_actor = co_await ex_actor::Spawn<HttpServerActor>(18098, &loop, &handler);

  exec::async_scope scope;
  scope.spawn(server_actor.template Send<&HttpServerActor::Run>());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  Info("Testing HTTP server...");

  struct sockaddr_in server_addr {};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(18098);
  inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

  int test_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(test_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    co_return TestResult{false, "Failed to connect"};
  }

  std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
  if (send(test_socket, request.c_str(), request.size(), 0) < 0) {
    close(test_socket);
    co_return TestResult{false, "Failed to send request"};
  }

  char buffer[4096] = {0};
  ssize_t bytes_read = recv(test_socket, buffer, sizeof(buffer) - 1, 0);
  if (bytes_read <= 0) {
    close(test_socket);
    co_return TestResult{false, "Failed to read response"};
  }
  std::string response(buffer, bytes_read);
  if (response.find("HTTP/1.1 200 OK") == std::string::npos) {
    close(test_socket);
    co_return TestResult{false, "Unexpected status code for /"};
  }
  if (response.find("Hello from ex-actor HTTP server!") == std::string::npos) {
    close(test_socket);
    co_return TestResult{false, "Missing expected body for /"};
  }
  close(test_socket);

  request = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
  test_socket = socket(AF_INET, SOCK_STREAM, 0);
  connect(test_socket, (struct sockaddr*)&server_addr, sizeof(server_addr));
  send(test_socket, request.c_str(), request.size(), 0);
  bytes_read = recv(test_socket, buffer, sizeof(buffer) - 1, 0);
  response = std::string(buffer, bytes_read);
  if (response.find("pong") == std::string::npos) {
    close(test_socket);
    co_return TestResult{false, "Missing pong for /ping"};
  }
  close(test_socket);

  request = "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\n\r\n";
  test_socket = socket(AF_INET, SOCK_STREAM, 0);
  connect(test_socket, (struct sockaddr*)&server_addr, sizeof(server_addr));
  send(test_socket, request.c_str(), request.size(), 0);
  bytes_read = recv(test_socket, buffer, sizeof(buffer) - 1, 0);
  response = std::string(buffer, bytes_read);
  if (response.find("HTTP/1.1 404") == std::string::npos) {
    close(test_socket);
    co_return TestResult{false, "Missing 404 for /nonexistent"};
  }
  close(test_socket);

  co_await server_actor.template Send<&HttpServerActor::Stop>();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  loop.Stop();
  ex_actor::Shutdown();

  result.success = true;
  Info("All async HTTP server tests passed!");
  co_return result;
}

TEST(KqueueHttpServerTest, BasicAsyncHttpServerTest) {
  auto result = ex::sync_wait(RunHttpServerTest());
  ASSERT_TRUE(result.has_value());
  auto& [success, message] = std::get<0>(*result);
  EXPECT_TRUE(success) << message;
}