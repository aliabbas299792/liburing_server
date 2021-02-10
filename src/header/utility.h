#ifndef UTILITY
#define UTILITY

#include <iostream> //for iostream
#include <cstdlib> //for exit
#include <cstring> //for strtok_r
#include <stdio.h> //perror and printf
#include <sys/stat.h> //fstat
#include <linux/fs.h> //for BLKGETSIZE64
#include <sys/ioctl.h> //for ioctl
#include <unordered_map> //for unordered_map
#include <vector> //for vector
#include <unistd.h> //read
#include <fcntl.h> //open
#include <sys/types.h> //O_RDONLY

typedef struct stat stat_struct;

// extern size_t global_malloced;

// void *CUSTOM_MALLOC(size_t num);
// void CUSTOM_FREE(void *ptr);

void fatal_error(std::string error_message); //fatal error helper function
long int get_file_size(int file_fd); //gets file size of the file descriptor passed in
std::unordered_map<std::string, std::string> read_config(); //helper function to read the config file (.config)
void sigint_handler(int sig_number); //handler used in main for handling SIGINT

//removes first n elements from a vector
template <typename T>
void remove_first_n_elements(std::vector<T> &data, int num_elements_to_remove){ //deals correctly with overlaps
  const auto size = data.size();
  const int overlap = size - 2*num_elements_to_remove;
  if(overlap > 0){
    std::memcpy(&data[0], &data[num_elements_to_remove], num_elements_to_remove);
    std::memcpy(&data[num_elements_to_remove], &data[2*num_elements_to_remove], overlap);
  }else{
    std::memcpy(&data[0], &data[num_elements_to_remove], size - num_elements_to_remove);
  }
  data.resize(size - num_elements_to_remove);
}

template <typename T>
void remove_first_n_elements(T *data, int length, T *&ret_buff, int num_elements_to_remove){ //deals correctly with overlaps
  const auto size = length;
  const int overlap = size - 2*num_elements_to_remove;
  if(overlap > 0){
    std::memcpy(&data[0], &data[num_elements_to_remove], num_elements_to_remove);
    std::memcpy(&data[num_elements_to_remove], &data[2*num_elements_to_remove], overlap);
  }else{
    std::memcpy(&data[0], &data[num_elements_to_remove], size - num_elements_to_remove);
  }
  char *temp_ptr = (T*)std::malloc(size - num_elements_to_remove);
  // char *temp_ptr = (T*)CUSTOM_MALLOC(size - num_elements_to_remove);
  free(ret_buff);
  // CUSTOM_FREE(ret_buff);
  ret_buff = temp_ptr;
  std::memcpy(ret_buff, data, size - num_elements_to_remove);
}

#endif