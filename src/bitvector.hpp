#ifndef BITVECTOR_HPP_INCLUDED
#define BITVECTOR_HPP_INCLUDED

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>

// Plain self-contained port of sascan/src/bitvector.h.
// Storage is a packed byte array; bit i lives in m_data[i>>3] at position (i&7).
struct bitvector {
  long m_alloc_bytes;
  unsigned char *m_data;

  // Allocate zeroed storage for `length` bits.
  bitvector(long length) {
    m_alloc_bytes = (length + 7) / 8;
    if (m_alloc_bytes <= 0) m_alloc_bytes = 1;
    m_data = new unsigned char[m_alloc_bytes];
    std::fill(m_data, m_data + m_alloc_bytes, (unsigned char)0);
  }

  // Load from disk; file is the raw packed bytes written by save().
  bitvector(const std::string &filename) {
    std::FILE *fp = std::fopen(filename.c_str(), "rb");
    if (!fp) {
      std::fprintf(stderr, "bitvector: cannot open %s for read\n", filename.c_str());
      std::exit(1);
    }
    std::fseek(fp, 0, SEEK_END);
    m_alloc_bytes = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    m_data = new unsigned char[m_alloc_bytes > 0 ? m_alloc_bytes : 1];
    if (m_alloc_bytes > 0 &&
        (long)std::fread(m_data, 1, m_alloc_bytes, fp) != m_alloc_bytes) {
      std::fprintf(stderr, "bitvector: short read on %s\n", filename.c_str());
      std::exit(1);
    }
    std::fclose(fp);
  }

  inline bool get(long i) const {
    return m_data[i >> 3] & (1 << (i & 7));
  }

  inline void set(long i) {
    m_data[i >> 3] |= (1 << (i & 7));
  }

  inline void reset(long i) {
    m_data[i >> 3] &= (unsigned char)~(1 << (i & 7));
  }

  void save(const std::string &filename) const {
    std::FILE *fp = std::fopen(filename.c_str(), "wb");
    if (!fp) {
      std::fprintf(stderr, "bitvector: cannot open %s for write\n", filename.c_str());
      std::exit(1);
    }
    if (m_alloc_bytes > 0 &&
        (long)std::fwrite(m_data, 1, m_alloc_bytes, fp) != m_alloc_bytes) {
      std::fprintf(stderr, "bitvector: short write on %s\n", filename.c_str());
      std::exit(1);
    }
    std::fclose(fp);
  }

  ~bitvector() {
    delete[] m_data;
  }
};

#endif
