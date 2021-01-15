#include "../header/callbacks.h"
#include "../header/utility.h"

#include <openssl/sha.h>
#include <openssl/evp.h>

void a_cb(int client_fd, server *tcp_server, void *custom_obj){ //the accept callback
  // std::cout << "Accepted new connection: " << client_fd << "\n";
}

std::string get_accept_header_value(std::string input) {
  input += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"; //concatenate magic string
  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1((const unsigned char*)input.c_str(), input.size(), hash);
  //for input of length 20, base64 output length is 28 (a bit over *4/3 - *4/3 for larger lengths)
  auto base64data = (char*)calloc(28+1, 1); //+1 for the terminating null that EVP_EncodeBlock adds on
  EVP_EncodeBlock((unsigned char*)base64data, hash, SHA_DIGEST_LENGTH);
  return base64data;
}

ulong get_ws_frame_length(const char *buffer){
  ulong packet_length = buffer[1] & 0x7f;
  if(packet_length == 126){
    packet_length = ntohs(*((u_short*)&((uchar*)buffer)[2])) + 2; //the 2 bytes extra needed to store the length are added
  }else if(packet_length == 127){
    packet_length = be64toh(*((u_long*)&((uchar*)buffer)[2])) + 8; //the 8 bytes extra needed to store the length are added, be64toh used because ntohl is 32 bit
  }
  return packet_length + 6; // +6 bytes for the header data
}

std::pair<int, std::vector<uchar>> decode_websocket_frame(std::vector<uchar> data){
  const uint fin = (data[0] & 0x80) == 0x80;
  const uint opcode = data[0] & 0xf;
  const uint mask = (data[1] & 0x80) == 0x80;

  if(!mask) return {-1, {}};

  int offset = 0;

  ulong length = data[1] & 0x7f;
  if(length == 126){
    offset = 2;
    length = ntohs(*((u_short*)&data[2]));
  }else if(length == 127){
    offset = 8;
    length = ntohs(*((u_long*)&data[2]));
  }

  const std::vector<uchar> masking_key{ data[2+offset], data[3+offset], data[4+offset], data[5+offset] };

  std::vector<uchar> decoded{};
  for(int i = 6+offset; i < data.size(); i++){
    decoded.push_back(data[i] ^ masking_key[(i - (6 + offset)) % 4]);
  }

  if(!fin) return {-2, decoded};
  
  return {-3, decoded};
}

bool is_valid_http_req(const char* buff, int length){
  if(length < 16) return false; //minimum size for valid HTTP request is 16 bytes
  const char *types[] = { "GET ", "POST ", "PUT ", "DELETE ", "PATCH " };
  u_char valid = 0x1f;
  for(int i = 0; i < 7; i++) //length of "DELETE " is 7 characters
    for(int j = 0; j < 5; j++) //5 different types
      if(i < strlen(types[j]) && (valid >> j) & 0x1 && types[j][i] != buff[i]) valid &= 0x1f ^ (1 << j);
  return valid;
}

void r_cb(int client_fd, char *buffer, unsigned int length, server *tcp_server, void *custom_obj){
  const auto basic_web_server = (web_server*)custom_obj;

  if(is_valid_http_req(buffer, length)){ //if not a valid HTTP req, then probably a websocket frame
    std::vector<std::string> headers;

    bool accept_bytes = false;
    std::string sec_websocket_key = "";

    const auto websocket_key_token = "Sec-WebSocket-Key: ";

    char *str = nullptr;
    char *saveptr = nullptr;
    char *buffer_str = buffer;
    while((str = strtok_r(((char*)buffer_str), "\r\n", &saveptr))){ //retrieves the headers
      std::string tempStr = std::string(str, strlen(str));
      
      if(tempStr.find("Range: bytes=") != std::string::npos) accept_bytes = true;
      if(tempStr.find("Sec-WebSocket-Key") != std::string::npos)
        sec_websocket_key = tempStr.substr(strlen(websocket_key_token));
      buffer_str = nullptr;
      headers.push_back(tempStr);
    }

    if(sec_websocket_key != ""){ //websocket connection callback
      const std::string accept_header_value = get_accept_header_value(sec_websocket_key);
      const auto resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept_header_value + "\r\n\r\n";

      std::vector<char> send_buffer(resp.size());
      std::memcpy(&send_buffer[0], resp.c_str(), resp.size());

      tcp_server->write_socket(client_fd, std::move(send_buffer));
      basic_web_server->websocket_connections.insert(client_fd);
    } else if(!strcmp(strtok_r((char*)headers[0].c_str(), " ", &saveptr), "GET")){ //get callback
      char *path = strtok_r(nullptr, " ", &saveptr);
      std::string processed_path = std::string(&path[1], strlen(path)-1);
      processed_path = processed_path == "" ? "public/index.html" : "public/"+processed_path;

      char *http_version = strtok_r(nullptr, " ", &saveptr);
      
      std::vector<char> send_buffer{};
      
      if((send_buffer = basic_web_server->read_file_web(processed_path, 200, accept_bytes)).size() != 0){
        tcp_server->write_socket(client_fd, std::move(send_buffer));
      }else{
        send_buffer = basic_web_server->read_file_web("public/404.html", 400);
        tcp_server->write_socket(client_fd, std::move(send_buffer));
      }
    } 
  } else if(basic_web_server->websocket_connections.count(client_fd)) { //this bit should be just websocket frames

    std::vector<uchar> frame{};

    auto packet_length = get_ws_frame_length(buffer);

    std::cout << "packet length: " << packet_length << "\n";

    if(basic_web_server->receiving_data.count(client_fd)){ //if there is already some pending data
      auto *pending_item = &basic_web_server->receiving_data[client_fd];

      if(pending_item->length == -1){
        if(pending_item->buffer.size() > 10){
          pending_item->length = get_ws_frame_length((const char*)&pending_item->buffer[0]);
        }else{ //assuming the received buffer and the pending buffer are at least 10 bytes long
          std::vector<char> temp_buffer(10); //we only need first 10 bytes for length
          temp_buffer.insert(temp_buffer.begin(), pending_item->buffer.begin(), pending_item->buffer.end());
          temp_buffer.insert(temp_buffer.begin(), buffer, buffer + ( 10 - pending_item->buffer.size() ));
          pending_item->length = get_ws_frame_length(&temp_buffer[0]);
        }
      }

      if(length + pending_item->buffer.size() == pending_item->length){
        pending_item->buffer.insert(pending_item->buffer.end(), buffer, buffer + length);
        frame = std::move(pending_item->buffer);
        basic_web_server->receiving_data.erase(client_fd);

      }else if(length + pending_item->buffer.size() < pending_item->length){ //too little data
        pending_item->buffer.insert(pending_item->buffer.end(), buffer, buffer + length); 

      }else{ //too much data
        long required_length = pending_item->length - pending_item->buffer.size();
        std::cout << pending_item->length << " " << required_length << " " << length << "\n";
        pending_item->buffer.insert(pending_item->buffer.end(), buffer, buffer + required_length);
        frame = std::move(pending_item->buffer);
        char *remaining_data = nullptr;
        remove_first_n_elements(buffer, (int)length, remaining_data, (int)required_length);

        pending_item->buffer.clear();
        pending_item->buffer.insert(pending_item->buffer.end(), remaining_data, remaining_data + (length - required_length)); //insert the remaining data
        pending_item->length = -1;
      }

    }else if((ulong)length == packet_length){ //is exactly how much was needed
      frame.insert(frame.begin(), buffer, buffer + length);
      
    }else{ //if there is no pending data, make this pending
      std::cout << "packet length inserted pending: " << packet_length << " " << length << "\n";
      auto *pending_item = &basic_web_server->receiving_data[client_fd];
      pending_item->length = packet_length;
      pending_item->buffer.insert(pending_item->buffer.end(), buffer, buffer + length); 
    }

    //by this point the frame has definitely been fully received
    
    if(frame.size()){
      auto processed_data = decode_websocket_frame(frame);
      std::cout << "frame fini\n";

      //for now finishes and prints last 200 bytes
      if(processed_data.first == -3){ // -3 is to indicate that it's done
        if(basic_web_server->websocket_frames.count(client_fd)){
          auto *vec_member = &basic_web_server->websocket_frames[client_fd];
          vec_member->insert(vec_member->end(), processed_data.second.begin(), processed_data.second.end());
          std::cout << std::string((char*)&(*vec_member)[vec_member->size() - 200], 200) << "\n";
          std::cout << "finished multi frame receive: " << vec_member->size() << "\n";
          basic_web_server->websocket_frames.erase(client_fd);
        }else{
          std::cout << std::string((char*)&processed_data.second[processed_data.second.size() - 200], 200) << "\n";
          std::cout << "finished single frame receive: " << processed_data.second.size() << "\n";
          //std::cout << std::string((char*)&processed_data.second[0], processed_data.second.size()) << "\n";
        }
      }else if(processed_data.first == -2){
        auto *vec_member = &basic_web_server->websocket_frames[client_fd];
        vec_member->insert(vec_member->begin(), processed_data.second.begin(), processed_data.second.end());
      }
    }

    /*if(processed_data.first != -3){
      auto *vec_member = &basic_web_server->receiving_data[client_fd];
      vec_member->insert(vec_member->end(), std::make_move_iterator(processed_data.second.begin()), std::make_move_iterator(processed_data.second.end()) );
    }

    if(processed_data.first == -2){
      //fatal_error("Mask bit not sent for socket...");
    }else if(processed_data.first == -1 || processed_data.first == -3){
      auto *vec_member = &basic_web_server->data_store[client_fd];
      vec_member->insert(vec_member->end(), std::make_move_iterator(processed_data.second.begin()), std::make_move_iterator(processed_data.second.end()) );
    }else{
      //basically make it receive the entire message and add it to a queue if it's not final etc etc
    }
    
    std::cout << "first: " << processed_data.first << "\n";
    std::cout << "length: " << buff_vec.size() << "\n\n\n";
    if(processed_data.first == -3){
      //std::cout << std::string((char*)(&processed_data.second[0]), processed_data.second.size()) << "\n";
      basic_web_server->data_store.erase(client_fd);
    }

    */
    if(basic_web_server->websocket_connections.count(client_fd))
      tcp_server->read_socket(client_fd);
  }
}

void w_cb(int client_fd, server *tcp_server, void *custom_obj){
  const auto basic_web_server = (web_server*)custom_obj;
  // std::cout << "write_socket callback\n";
  if(basic_web_server->websocket_connections.count(client_fd))
    tcp_server->read_socket(client_fd);
  else
    tcp_server->close_socket(client_fd); //for web requests you close the socket right after
}