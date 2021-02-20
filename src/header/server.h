#ifndef SERVER
#define SERVER

#include <cstring> //for memset and strtok

#include <stdio.h> //perror and printf
#include <netdb.h> //for networking stuff like addrinfo

#include <sys/syscall.h> //syscall stuff parameters (as in like __NR_io_uring_enter/__NR_io_uring_setup)
#include <sys/mman.h> //for mmap
#include <sys/eventfd.h>

#include <liburing.h> //for liburing

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <queue>
#include <vector> //for vectors
#include <iostream> //for string and iostream stuff
#include <unordered_map>
#include <unordered_set>
#include <set> //ordered set for freed indexes, I believe it is sorted in ascending order which is exactly what we want
#include <chrono>
#include <mutex>

//I don't want to type out the parameters twice, so I don't
#define ACCEPT_CB_PARAMS int client_idx, server<T> *tcp_server, void *custom_obj
#define READ_CB_PARAMS int client_idx, char* buffer, unsigned int length, ulong custom_info, server<T> *tcp_server, void *custom_obj
#define WRITE_CB_PARAMS int client_idx, ulong custom_info, server<T> *tcp_server, void *custom_obj
#define EVENT_CB_PARAMS server<T> *tcp_server, void *custom_obj

constexpr int BACKLOG = 10; //max number of connections pending acceptance
constexpr int READ_SIZE = 8192; //how much one read request should read
constexpr int QUEUE_DEPTH = 256; //the maximum number of events which can be submitted to the io_uring submission queue ring at once, you can have many more pending requests though
constexpr int READ_BLOCK_SIZE = 8192; //how much to read from a file at once

enum class event_type{ ACCEPT, ACCEPT_READ, ACCEPT_WRITE, READ, WRITE, EVENTFD };
enum class server_type { TLS, NON_TLS };

template<server_type T>
class server_base; //forward declaration

template<server_type T>
class server;

//the wolfSSL callbacks
int tls_recv_helper(std::unordered_map<int, std::vector<char>> *recv_data, server<server_type::TLS> *tcp_server, char *buff, int sz, int client_socket, bool accept);
int tls_recv(WOLFSSL* ssl, char* buff, int sz, void* ctx);
int tls_send(WOLFSSL* ssl, char* buff, int sz, void* ctx);

template<server_type T>
using accept_callback = void (*)(ACCEPT_CB_PARAMS);

template<server_type T>
using read_callback = void(*)(READ_CB_PARAMS);

template<server_type T>
using write_callback = void(*)(WRITE_CB_PARAMS);

template<server_type T>
using event_callback = void(*)(EVENT_CB_PARAMS);

struct request {
  event_type event;
  int client_idx{};
  int ID{};
  size_t written{}; //how much written so far
  size_t total_length{}; //how much data is in the request, in bytes
  char *buffer = nullptr;
  std::vector<char> send_data{};
  std::vector<char> read_data{};
  size_t read_amount{}; //how much has been read (in case of multi read requests)
  uint64_t custom_info{}; //any custom info you want to attach to the request
};

struct multi_write {
  multi_write(std::vector<char> &&buff, int uses) : buff(buff), uses(uses) {}
  std::vector<char> buff;
  int uses{}; //this should be decremented each time you would normally delete this object, when it reaches 0, then delete
};

struct write_data {
  write_data(std::vector<char> &&buff) : buff(buff) {}
  write_data(multi_write *multi_write_data) : multi_write_data(multi_write_data) {}
  multi_write *multi_write_data = nullptr; //if not null then buff should be empty, and data should be in the multi_write pointer
  std::vector<char> buff;
  int last_written = -1;
  ~write_data(){
    if(multi_write_data){
      multi_write_data->uses--;
      if(multi_write_data->uses == 0)
        delete multi_write_data;
    }
  }
};

struct client_base {
  int id{};
  int sockfd{};
  std::queue<write_data> send_data{};
  ulong custom_info = -1; //default custom_info is -1
};

template<server_type T>
struct client: client_base {};

template<>
struct client<server_type::NON_TLS>: client_base {};

template<>
struct client<server_type::TLS>: client_base {
    WOLFSSL *ssl = nullptr;
    int accept_last_written = -1;
    std::vector<char> recv_data{};
};

template<server_type T>
class server_base {
  protected:
    accept_callback<T> accept_cb = nullptr;
    read_callback<T> read_cb = nullptr;
    write_callback<T> write_cb = nullptr;
    event_callback<T> event_cb = nullptr;

    int thread_id = -1;
    io_uring ring;
    void *custom_obj; //it can be anything

    std::unordered_set<int> active_connections{};
    std::set<int> freed_indexes{}; //using a set to store free indexes instead
    std::vector<client<T>> clients{};

    void add_tcp_accept_req();

    //need it protected rather than private, since need to access from children
    int add_write_req(int client_idx, event_type event, char *buffer, unsigned int length); //this is for the case you want to write a buffer rather than a vector
    //used internally for sending messages
    int add_read_req(int client_idx, event_type event); //adds a read request to the io_uring ring

    void custom_read_req(int fd, size_t to_read, int client_idx, std::vector<char> &&buff = {}, size_t read_amount = 0);
    
    int setup_client(int client_idx);

    void event_read(); //will set a read request for the eventfd

    bool running_server = false;
  private:
    int event_fd = eventfd(0, 0); //used to awaken this thread for some event

    int listener_fd = 0;

    int add_accept_req(int listener_fd, sockaddr_storage *client_address, socklen_t *client_address_length); //adds an accept request to the io_uring ring
    //used in the req_event_handler functions for accept requests
    sockaddr_storage client_address{};
    socklen_t client_address_length = sizeof(client_address);

    int setup_listener(int port); //sets up the listener socket

    //needed to synchronize the multiple server threads
    static std::mutex init_mutex;
    static int shared_ring_fd; //pointer to a single io_uring ring fd, who's async backend is shared
    static int current_max_id; //max id of thread
  public:
    server_base(int listen_port);
    void start(); //function to start the server

    void read_connection(int client_idx, ulong custom_info = 0);

    void notify_event();
};

template<>
class server<server_type::NON_TLS>: public server_base<server_type::NON_TLS> {
  private:
    friend class server_base;

    int add_write_req_continued(request *req, int offset); //only used for when writev didn't write everything
    
    void server_loop(); //the main server loop
  public:
    server(int listen_port, 
      accept_callback<server_type::NON_TLS> a_cb = nullptr,
      read_callback<server_type::NON_TLS> r_cb = nullptr,
      write_callback<server_type::NON_TLS> w_cb = nullptr,
      event_callback<server_type::NON_TLS> e_cb = nullptr,
      void *custom_obj = nullptr
    );
    
    template<typename U>
    void broadcast_message(U begin, U end, int num_clients, std::vector<char> &&buff){
      if(num_clients > 0){
        auto data = new multi_write(std::move(buff), num_clients);

        for(auto client_idx_ptr = begin; client_idx_ptr != end; client_idx_ptr++){
          auto &client = clients[*client_idx_ptr];
          client.send_data.emplace(data);
          if(client.send_data.size() == 1) //only adds a write request in the case that the queue was empty before this
            add_write_req(*client_idx_ptr, event_type::WRITE, &(data->buff[0]), data->buff.size());
        }
      }
    }

    void write_connection(int client_idx, std::vector<char> &&buff, ulong custom_info = 0); //writing depends on TLS or SSL, unlike read
    void close_connection(int client_idx); //closing depends on what resources need to be freed
};

template<>
class server<server_type::TLS>: public server_base<server_type::TLS> {
  private:
    friend int tls_recv_helper(server<server_type::TLS> *tcp_server, int client_idx, char *buff, int sz, bool accept);
    friend int tls_recv(WOLFSSL* ssl, char* buff, int sz, void* ctx);
    friend int tls_send(WOLFSSL* ssl, char* buff, int sz, void* ctx);

    friend class server_base;
    void tls_accept(int client_socket);
    
    void server_loop(); //the main server loop

    WOLFSSL_CTX *wolfssl_ctx = nullptr;
  public:
    server(
      int listen_port,
      std::string fullchain_location,
      std::string pkey_location,
      accept_callback<server_type::TLS> a_cb = nullptr,
      read_callback<server_type::TLS> r_cb = nullptr,
      write_callback<server_type::TLS> w_cb = nullptr,
      event_callback<server_type::TLS> e_cb = nullptr,
      void *custom_obj = nullptr
    );
    
    template<typename U>
    void broadcast_message(U begin, U end, int num_clients, std::vector<char> &&buff){
      if(num_clients > 0){
        auto data = new multi_write(std::move(buff), num_clients);

        for(auto client_idx_ptr = begin; client_idx_ptr != end; client_idx_ptr++){
          auto &client = clients[*client_idx_ptr];
          client.send_data.emplace(data);
          if(client.send_data.size() == 1) //only adds a write request in the case that the queue was empty before this
            wolfSSL_write(client.ssl, &(data->buff[0]), data->buff.size());
        }
      }
    }

    void write_connection(int client_idx, std::vector<char> &&buff, ulong custom_info = 0); //writing depends on TLS or SSL, unlike read
    void close_connection(int client_idx); //closing depends on what resources need to be freed
};

#include "../tcp_server/server_base.tcc" //template implementation file

#endif