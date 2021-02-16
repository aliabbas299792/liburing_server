#include "../header/web_server.h"
#include "../header/utility.h"

template<server_type T>
web_server<T>::web_server(){
  io_uring_queue_init(QUEUE_DEPTH, &ring, 0); //no flags, setup the queue
}

template<server_type T>
bool web_server<T>::get_process(std::string &path, bool accept_bytes, const std::string& sec_websocket_key, int client_idx, server<T> *tcp_server){
  char *saveptr = nullptr;
  const char* token = strtok_r((char*)path.c_str(), "/", &saveptr);
  const char* subdir = token ? token : "";

  if( (strlen(subdir) == 2 && strncmp(subdir, "ws", 2) == 0)  && sec_websocket_key != ""){
    websocket_accept_read_cb(sec_websocket_key, path.substr(2), client_idx, tcp_server);
    return true;
  }else{
    path = path == "" ? "public/index.html" : "public/"+path;
    
    std::vector<char> send_buffer{};
    
    if((send_buffer = read_file_web(path, 200, accept_bytes)).size() != 0){
      tcp_server->write_connection(client_idx, std::move(send_buffer));
      return true;
    }
    return false;
  }
}

template<server_type T>
bool web_server<T>::is_valid_http_req(const char* buff, int length){
  if(length < 16) return false; //minimum size for valid HTTP request is 16 bytes
  const char *types[] = { "GET ", "POST ", "PUT ", "DELETE ", "PATCH " };
  u_char valid = 0x1f;
  for(int i = 0; i < 7; i++) //length of "DELETE " is 7 characters
    for(int j = 0; j < 5; j++) //5 different types
      if(i < strlen(types[j]) && (valid >> j) & 0x1 && types[j][i] != buff[i]) valid &= 0x1f ^ (1 << j);
  return valid;
}

template<server_type T>
std::string web_server<T>::get_content_type(std::string filepath){
  char *file_extension_data = (char*)filepath.c_str();
  std::string file_extension = "";
  char *saveptr = nullptr;
  while((file_extension_data = strtok_r(file_extension_data, ".", &saveptr))){
    file_extension = file_extension_data;
    file_extension_data = nullptr;
  }

  if(file_extension == "html" || file_extension == "htm")
    return "Content-Type: text/html\r\n";
  if(file_extension == "css")
    return "Content-Type: text/css\r\n";
  if(file_extension == "js")
    return "Content-Type: text/javascript\r\n";
  if(file_extension == "opus")
    return "Content-Type: audio/opus\r\n";
  if(file_extension == "mp3")
    return "Content-Type: audio/mpeg\r\n";
  if(file_extension == "mp4")
    return "Content-Type: video/mp4\r\n";
  if(file_extension == "gif")
    return "Content-Type: image/gif\r\n";
  if(file_extension == "png")
    return "Content-Type: image/png\r\n";
  if(file_extension == "jpg" || file_extension == "jpeg")
    return "Content-Type: image/jpeg\r\n";
  if(file_extension == "txt")
    return "Content-Type: text/plain\r\n";
  return "Content-Type: application/octet-stream\r\n";
}

template<server_type T>
int web_server<T>::read_file(std::string filepath, std::vector<char>& buffer, int reserved_bytes){
  int file_fd = open(filepath.c_str(), O_RDONLY);
  if(file_fd < 0) return -1;

  const auto size = get_file_size(file_fd);
  int read_bytes = 0;

  buffer.resize(reserved_bytes + size);

  while(read_bytes != size){
    io_uring_sqe *sqe = io_uring_get_sqe(&ring); //get a valid SQE (correct index and all)

    io_uring_prep_read(sqe, file_fd, &buffer[reserved_bytes], size - read_bytes, read_bytes); //don't read at an offset
    io_uring_submit(&ring); //submits the event

    io_uring_cqe *cqe;
    char ret = io_uring_wait_cqe(&ring, &cqe);
    read_bytes += cqe->res;

    io_uring_cqe_seen(&ring, cqe); //mark this CQE as seen
  }

  close(file_fd);
  
  return size;
}

template<server_type T>
std::vector<char> web_server<T>::read_file_web(std::string filepath, int responseCode, bool accept_bytes){
  auto header_first_line = "";
  
  switch(responseCode){
    case 200:
      header_first_line = "HTTP/1.1 200 OK\r\n";
      break;
    default:
      header_first_line = "HTTP/1.1 404 Not Found\r\n";
  }

  const auto content_type = get_content_type(filepath);
  const auto reserved_bytes = 200; //I'm estimating, header is probably going to be up to 200 bytes
  //char *buffer = nullptr;

  std::vector<char> buffer{};
  const auto size = read_file(filepath, buffer, reserved_bytes);

  if(size < 0) return std::vector<char>{}; //return an empty array if the file was not found (negative length)

  const auto content_length = std::to_string(size);

  std::string headers = "";
  if(accept_bytes){
    headers = header_first_line + content_type + "Accept-Ranges: bytes\r\nContent-Length: " + content_length + "\r\nRange: bytes=0-" + content_length + "/" + content_length + "\r\nConnection:";
  }else{
    headers = header_first_line + content_type + "Content-Length: " + content_length + "\r\nConnection:";
  }

  const auto header_last = "Keep-Alive\r\n\r\n"; //last part of the header
  
  std::memset(&buffer[0], 32, reserved_bytes); //this sets the entire header section in the buffer to be whitespace
  std::memcpy(&buffer[0], headers.c_str(), headers.size()); //this copies the first bit of the header to the beginning of the buffer
  std::memcpy(&buffer[reserved_bytes-strlen(header_last)], header_last, strlen(header_last)); //this copies the last bit to the end of the reserved section

  return buffer;
}