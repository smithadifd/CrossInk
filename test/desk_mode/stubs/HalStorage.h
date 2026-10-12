#pragma once

// Just enough of HalStorage for Bitmap.h to compile: the Desk mode header check only needs BmpHeader.
#include <cstddef>
#include <cstdint>

class HalFile {
 public:
  void close() {}
  size_t fileSize() const { return 0; }
  int read() { return -1; }
  int read(void*, size_t) { return 0; }
  bool seek(size_t) { return false; }
  bool seekCur(int64_t) { return false; }
  explicit operator bool() const { return false; }
};

using FsFile = HalFile;
