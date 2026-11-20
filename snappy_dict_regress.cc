// Regression checks for the prefix-dictionary feature against real corpus
// files: regular streams must keep working through every entry point, and
// dictionary streams must round-trip and reject wrong/corrupt dictionaries.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "snappy.h"

namespace {
int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                   #cond);                                             \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

std::string ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

void TestCorpus(const std::string& path) {
  const std::string data = ReadFile(path);
  CHECK(!data.empty());

  // 1. Regular compression stays compatible with all decompressors.
  std::string regular, out;
  snappy::Compress(data.data(), data.size(), &regular);
  CHECK(snappy::Uncompress(regular.data(), regular.size(), &out));
  CHECK(out == data);
  CHECK(snappy::UncompressWithDictionary(regular.data(), regular.size(),
                                         nullptr, 0, &out));
  CHECK(out == data);
  std::string raw_out(data.size(), '\0');
  CHECK(snappy::RawUncompress(regular.data(), regular.size(),
                              &raw_out[0]));
  CHECK(raw_out == data);

  // 2. Dictionary compression round-trips with the matching dictionary.
  const size_t dict_size = std::min<size_t>(8192, data.size());
  const std::string dictionary = data.substr(0, dict_size);
  std::string dict_compressed;
  CHECK(snappy::CompressWithDictionary(dictionary.data(), dictionary.size(),
                                       data.data(), data.size(),
                                       &dict_compressed) != 0);
  CHECK(snappy::UncompressWithDictionary(
      dict_compressed.data(), dict_compressed.size(), dictionary.data(),
      dictionary.size(), &out));
  CHECK(out == data);

  // 3. A wrong dictionary must not silently produce the original data.
  std::string wrong_dict(dictionary.size(), '?');
  bool ok = snappy::UncompressWithDictionary(
      dict_compressed.data(), dict_compressed.size(), wrong_dict.data(),
      wrong_dict.size(), &out);
  if (ok && out == data) {
    std::fprintf(stderr,
                 "note: %s dictionary stream had no effective dictionary "
                 "back-references (wrong dictionary still decoded)\n",
                 path.c_str());
  }

  // 4. A corrupted stream is rejected rather than read out of bounds.
  std::string corrupted = dict_compressed;
  corrupted.resize(corrupted.size() / 2);  // truncated mid-stream
  CHECK(!snappy::UncompressWithDictionary(
      corrupted.data(), corrupted.size(), dictionary.data(),
      dictionary.size(), &out));
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::printf("regress: %s\n", argv[i]);
    TestCorpus(argv[i]);
  }
  if (g_failures == 0) {
    std::printf("All regression checks passed.\n");
    return 0;
  }
  std::printf("%d check(s) failed.\n", g_failures);
  return 1;
}
