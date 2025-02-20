#pragma once

#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "nvm/nvmem.h"
#define LOGCAP 30 * MB

namespace leveldb {
namespace silkstore {
class NvmManager {
 private:
  const char* nvm_file_;
  // logCap_ is used to Divide a part of the memory for logging, default value
  // is 30*MB
  size_t logCap_;
  size_t index_;
  size_t cap_;
  char* data_;
  std::deque<std::pair<size_t, size_t>> memUsage;
  std::mutex mtx;
  void init();

 public:
  NvmManager();
  explicit NvmManager(const char* nvm_file, size_t size = GB);
  ~NvmManager();
  // allocate new nvmem
  Nvmem* allocate(size_t cap = 30 * MB);
  // using to recovery nvm table
  Nvmem* reallocate(size_t offset, size_t cap);
  std::string getNvmInfo();
  bool recovery(const std::vector<size_t>& records);
  void free(char* address);
};

}  // namespace silkstore
}  // namespace leveldb
