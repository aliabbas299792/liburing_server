#include "header/utility.h"
#include "header/web_server/central_web_server.h"

#include <thread>
#include <sys/eventfd.h>

int main(){
  signal(SIGINT, sigint_handler); //signal handler for when Ctrl+C is pressed
  signal(SIGPIPE, SIG_IGN); //signal handler for when a connection is closed while writing

  central_web_server::get_instance().start(".config");
  
  return 0;
}