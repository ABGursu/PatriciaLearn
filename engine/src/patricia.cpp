#include "search.h"
#include "uci.h"
#include <memory>
#include <stdio.h>
#include <fstream>
#include <cstdarg>

// Simple debug log implementation - writes all output to both stdout and debug.log
// This does NOT redirect stdout, so UCI communication works normally
namespace Debug {
  static std::ofstream log_file;
  static bool enabled = false;  // Default: disabled (UCI option controls this)
  
  void set_enabled(bool value) {
    enabled = value;
    if (!enabled && log_file.is_open()) {
      log_file.close();
    }
  }
  
  void init() {
    if (enabled) {
      log_file.open("debug.log", std::ios::out | std::ios::app);
    }
  }
  
  void logf(const char* format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    if (len > 0) {
      // Write to stdout (for UCI communication) - stdout is NOT redirected
      fputs(buffer, stdout);
      fflush(stdout);
      
      // Also write to log file if enabled
      if (enabled && log_file.is_open()) {
        log_file << buffer;
        log_file.flush();
      }
    }
  }
  
  void log(const char* str) {
    // Write to stdout (for UCI communication) - stdout is NOT redirected
    fputs(str, stdout);
    fflush(stdout);
    
    // Also write to log file if enabled
    if (enabled && log_file.is_open()) {
      log_file << str;
      log_file.flush();
    }
  }
  
  void close() {
    if (log_file.is_open()) {
      log_file.close();
    }
  }
}

int main(int argc, char *argv[]) {
  Position position;
  std::unique_ptr<ThreadInfo> thread_info = std::make_unique<ThreadInfo>();

  init_LMR();
  init_bbs();

  if (argc > 1) {
    if (std::string(argv[1]) == "bench") {
      bench(position, *thread_info);
      std::exit(0);
    }
    else if (std::string(argv[1]) == "perft"){
      set_board(position, *thread_info, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
      perft(std::atoi(argv[2]), position, true, *thread_info);
      std::exit(0);
    }
  }

  uci(*thread_info, position);
  return 0;
}