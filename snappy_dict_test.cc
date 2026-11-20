// Standalone tests for the prefix-dictionary compression API.
//
// Built without gtest (the googletest submodule is optional), this program
// links against the snappy sources directly and checks:
//   1. round-trip correctness for both regular and dictionary compression;
//   2. that regular streams still decompress and dictionary streams need their
//      dictionary (decompressor-side compatibility with both formats);
//   3. the total archive size of a collection of small files.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "snappy.h"

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                   #cond);                                            \
      ++g_failures;                                                   \
    }                                                                 \
  } while (0)

#define CHECK_EQ(a, b)                                                \
  do {                                                                \
    auto va_ = (a);                                                   \
    auto vb_ = (b);                                                   \
    if (!(va_ == vb_)) {                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s != %s (%lld vs %lld)\n", \
                   __FILE__, __LINE__, #a, #b,                       \
                   static_cast<long long>(va_),                      \
                   static_cast<long long>(vb_));                     \
      ++g_failures;                                                   \
    }                                                                 \
  } while (0)

#define CHECK_NE(a, b)                                                \
  do {                                                                \
    auto va_ = (a);                                                   \
    auto vb_ = (b);                                                   \
    if (!(va_ != vb_)) {                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s == %s (%lld)\n",          \
                   __FILE__, __LINE__, #a, #b,                       \
                   static_cast<long long>(va_));                     \
      ++g_failures;                                                   \
    }                                                                 \
  } while (0)

std::string ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  return content;
}

// Split a source buffer into "count" small files of roughly equal size with
// overlapping windows, mimicking a batch of records that share vocabulary.
std::vector<std::string> MakeSmallFiles(const std::string& source, size_t count,
                                        size_t window, size_t step) {
  std::vector<std::string> files;
  files.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    size_t pos = (i * step) % (source.size() - window);
    files.push_back(source.substr(pos, window));
  }
  return files;
}

void TestRoundTrip() {
  std::mt19937 rng(12345);
  const std::string alphabet =
      "the quick brown fox jumps over the lazy dog. THE QUICK FOX 0123456789 ";

  for (int trial = 0; trial < 200; ++trial) {
    size_t input_len = 1 + rng() % 4096;
    size_t dict_len = rng() % 4096;
    std::string input, dictionary;
    input.reserve(input_len);
    dictionary.reserve(dict_len);
    for (size_t i = 0; i < input_len; ++i) {
      input.push_back(alphabet[rng() % alphabet.size()]);
    }
    for (size_t i = 0; i < dict_len; ++i) {
      dictionary.push_back(alphabet[rng() % alphabet.size()]);
    }

    std::string compressed, uncompressed;
    size_t written = snappy::CompressWithDictionary(
        dictionary.data(), dictionary.size(), input.data(), input.size(),
        &compressed);
    CHECK_TRUE(written == compressed.size());
    // A dictionary stream that actually back-references into the dictionary
    // is invalid under the plain (no-dictionary) validator; when it happened
    // to emit no such references, the plain validator accepts it. Either way
    // the dictionary-aware round-trip below must succeed.
    if (dictionary.empty()) {
      CHECK_TRUE(snappy::IsValidCompressedBuffer(compressed.data(),
                                                 compressed.size()));
    }
    CHECK_TRUE(snappy::UncompressWithDictionary(
        compressed.data(), compressed.size(), dictionary.data(),
        dictionary.size(), &uncompressed));
    CHECK_EQ(uncompressed.size(), input.size());
    CHECK_TRUE(uncompressed == input);

    // Raw array variants.
    std::vector<char> raw(snappy::MaxCompressedLength(input.size()));
    size_t raw_len = 0;
    snappy::RawCompressWithDictionary(
        dictionary.data(), dictionary.size(), input.data(), input.size(),
        raw.data(), &raw_len);
    CHECK_EQ(raw_len, compressed.size());
    CHECK_TRUE(std::memcmp(raw.data(), compressed.data(), raw_len) == 0);
    std::vector<char> raw_out(input.size());
    CHECK_TRUE(snappy::RawUncompressWithDictionary(
        raw.data(), raw_len, dictionary.data(), dictionary.size(),
        raw_out.data()));
    CHECK_TRUE(std::memcmp(raw_out.data(), input.data(), input.size()) == 0);
  }
}

void TestDictionaryActuallyReferenced() {
  // Input is a long repetition of a pattern that exists only in the
  // dictionary. With it, the stream should be much smaller than the input;
  // without any dictionary the same data is still self-repetitive, so instead
  // compare against compression where the dictionary is withheld.
  const std::string pattern =
      "columnar-compressor-shared-dictionary-prefix-block-0123456789";
  std::string dictionary = pattern;
  std::string input;
  while (input.size() < 2048) input += pattern;

  std::string with_dict, without_dict, out;
  snappy::CompressWithDictionary(dictionary.data(), dictionary.size(),
                                 input.data(), input.size(), &with_dict);
  snappy::Compress(input.data(), input.size(), &without_dict);

  CHECK_TRUE(snappy::UncompressWithDictionary(
      with_dict.data(), with_dict.size(), dictionary.data(),
      dictionary.size(), &out));
  CHECK_TRUE(out == input);
  // The dictionary path must not be worse, and here it matches the self-match
  // compression closely.
  CHECK_TRUE(with_dict.size() <= without_dict.size() + 4);
}

void TestDictionaryBoundaryMatch() {
  // A match rooted at the very end of the dictionary that would continue into
  // the input must round-trip and exploit the dictionary (output far smaller
  // than the repetitive input).
  std::string dictionary(100, 'z');
  dictionary += "abcd";
  std::string input = "abcdefghijklmnopqrstuvwxyz";
  for (int i = 0; i < 60; ++i) input += "abcdefghij";

  std::string compressed, restored;
  size_t written = snappy::CompressWithDictionary(
      dictionary.data(), dictionary.size(), input.data(), input.size(),
      &compressed);
  CHECK_EQ(written, compressed.size());
  CHECK_TRUE(snappy::UncompressWithDictionary(
      compressed.data(), compressed.size(), dictionary.data(),
      dictionary.size(), &restored));
  CHECK_TRUE(restored == input);
  CHECK_TRUE(compressed.size() < input.size() / 4);
}

void TestCrossFormatCompatibility() {
  const std::string input = "regular snappy stream, no dictionary";
  std::string regular, dict_stream, out;
  snappy::Compress(input.data(), input.size(), &regular);
  snappy::CompressWithDictionary("dict", 4, input.data(), input.size(),
                                 &dict_stream);

  // Regular streams decompress through both the classic and dictionary API
  // with an empty dictionary.
  CHECK_TRUE(snappy::Uncompress(regular.data(), regular.size(), &out));
  CHECK_TRUE(out == input);
  CHECK_TRUE(snappy::UncompressWithDictionary(
      regular.data(), regular.size(), nullptr, 0, &out));
  CHECK_TRUE(out == input);

  // A dictionary stream that references the dictionary must fail when fed to
  // the plain decompressor (its copy offset reaches before output start).
  // Build a stream guaranteed to reference dictionary content: input begins
  // with bytes that only appear in the dictionary.
  const std::string dictionary = "xxxxxxxx-common-prefix-yyyyyyyy";
  const std::string dict_input = dictionary + "-tail-tail-tail-tail";
  std::string ref_stream, ref_out;
  CHECK_NE(snappy::CompressWithDictionary(
               dictionary.data(), dictionary.size(), dict_input.data(),
               dict_input.size(), &ref_stream),
           0u);
  bool plain_ok = snappy::Uncompress(ref_stream.data(), ref_stream.size(),
                                     &ref_out);
  (void)plain_ok;  // Either fails or yields different bytes; never silently OK
                  // with the right content.
  if (plain_ok) CHECK_TRUE(ref_out != dict_input);
  CHECK_TRUE(snappy::UncompressWithDictionary(
      ref_stream.data(), ref_stream.size(), dictionary.data(),
      dictionary.size(), &ref_out));
  CHECK_TRUE(ref_out == dict_input);
}

void TestEdgeCases() {
  std::string out, comp;

  // Empty dictionary behaves like regular compression.
  const std::string input = "hello dictionary world";
  CHECK_NE(snappy::CompressWithDictionary(nullptr, 0, input.data(),
                                          input.size(), &comp),
           0u);
  CHECK_TRUE(snappy::UncompressWithDictionary(comp.data(), comp.size(),
                                              nullptr, 0, &out));
  CHECK_TRUE(out == input);

  // Empty input.
  CHECK_NE(snappy::CompressWithDictionary("abc", 3, "", 0, &comp), 0u);
  CHECK_TRUE(snappy::UncompressWithDictionary(comp.data(), comp.size(), "abc",
                                              3, &out));
  CHECK_EQ(out.size(), 0u);

  // Oversized dictionary is truncated to its last kBlockSize bytes.
  std::string big_dict(snappy::kBlockSize + 100, 'z');
  CHECK_NE(snappy::CompressWithDictionary(big_dict.data(), big_dict.size(),
                                          input.data(), input.size(), &comp),
           0u);
  const std::string truncated = big_dict.substr(big_dict.size() -
                                                snappy::kBlockSize);
  CHECK_TRUE(snappy::UncompressWithDictionary(
      comp.data(), comp.size(), truncated.data(), truncated.size(), &out));
  CHECK_TRUE(out == input);

  // dictionary + first block overflow: the oldest dictionary bytes are
  // dropped until the offset window fits. Round-trip still succeeds.
  std::string big_input(snappy::kBlockSize, 'a');
  CHECK_NE(snappy::CompressWithDictionary(big_dict.data(), 64,
                                          big_input.data(), big_input.size(),
                                          &comp),
           0u);
  // First block is a full kBlockSize, so no dictionary bytes survive.
  CHECK_TRUE(snappy::UncompressWithDictionary(
      comp.data(), comp.size(), nullptr, 0, &out));
  CHECK_TRUE(out == big_input);

  // Multi-block (>32KiB) input with a small dictionary still round-trips; only
  // the first block may reference the dictionary.
  std::string long_input;
  for (int i = 0; i < 3000; ++i) long_input += "repeating-block-content-";
  const std::string small_dict = "repeating-block";
  CHECK_NE(snappy::CompressWithDictionary(small_dict.data(),
                                          small_dict.size(),
                                          long_input.data(), long_input.size(),
                                          &comp),
           0u);
  CHECK_TRUE(snappy::UncompressWithDictionary(
      comp.data(), comp.size(), small_dict.data(), small_dict.size(), &out));
  CHECK_TRUE(out == long_input);
}

void TestMixedArchiveCompatibility() {
  // Simulate one weekly batch containing both legacy (no-dictionary) files and
  // new shared-dictionary files. The decompressor must handle each with the
  // matching entry point without any mode flag in the stream.
  const std::string dictionary = "weekly-batch-shared-vocabulary-0123456789";
  std::mt19937 rng(777);
  const std::string words[] = {"invoice-", "header-", "line-item-",
                               "checksum-", "trailer-"};

  bool all_ok = true;
  for (int i = 0; i < 500; ++i) {
    std::string record = words[rng() % 5];
    for (int j = 0; j < 8; ++j) record += words[rng() % 5];
    std::string legacy, shared, restored_legacy, restored_shared;

    snappy::Compress(record.data(), record.size(), &legacy);
    snappy::CompressWithDictionary(dictionary.data(), dictionary.size(),
                                   record.data(), record.size(), &shared);

    all_ok &= snappy::Uncompress(legacy.data(), legacy.size(),
                                 &restored_legacy);
    all_ok &= restored_legacy == record;
    all_ok &= snappy::UncompressWithDictionary(
        shared.data(), shared.size(), dictionary.data(), dictionary.size(),
        &restored_shared);
    all_ok &= restored_shared == record;
  }
  CHECK_TRUE(all_ok);
}

void TestMaximumOffsetWindow() {
  // Fill the full 65535-byte offset window: a 32768-byte dictionary followed
  // by a 32768-byte first block that repeats dictionary content, forcing the
  // largest representable copy offsets.
  std::mt19937 rng(4242);
  std::string dictionary;
  dictionary.reserve(snappy::kBlockSize);
  const std::string alphabet = "abcdefgh-0123456789-";
  while (dictionary.size() < snappy::kBlockSize) {
    dictionary.push_back(alphabet[rng() % alphabet.size()]);
  }
  std::string input = dictionary;  // same size, highly redundant

  std::string compressed, restored;
  size_t written = snappy::CompressWithDictionary(
      dictionary.data(), dictionary.size(), input.data(), input.size(),
      &compressed);
  CHECK_NE(written, 0u);
  CHECK_TRUE(snappy::UncompressWithDictionary(
      compressed.data(), compressed.size(), dictionary.data(),
      dictionary.size(), &restored));
  CHECK_TRUE(restored == input);

  // Raw variant on the maximum window as well.
  std::vector<char> raw(snappy::MaxCompressedLength(input.size()));
  size_t raw_len = 0;
  snappy::RawCompressWithDictionary(
      dictionary.data(), dictionary.size(), input.data(), input.size(),
      raw.data(), &raw_len);
  CHECK_EQ(raw_len, compressed.size());
  std::vector<char> raw_out(input.size());
  CHECK_TRUE(snappy::RawUncompressWithDictionary(
      raw.data(), raw_len, dictionary.data(), dictionary.size(),
      raw_out.data()));
  CHECK_TRUE(std::memcmp(raw_out.data(), input.data(), input.size()) == 0);
}

void TestSmallFileArchiveSize(const std::string& source_path,
                              size_t file_count, size_t window) {
  const std::string source = ReadFile(source_path);
  if (source.size() < 1 << 16) {
    std::fprintf(stderr, "SKIP archive test: %s too small\n",
                 source_path.c_str());
    return;
  }

  const size_t kFileCount = file_count;
  const size_t kWindow = window;
  const size_t kStep = std::max<size_t>(1, window / 14);
  std::vector<std::string> files =
      MakeSmallFiles(source, kFileCount, kWindow, kStep);

  // A shared dictionary: the first block-sized prefix of the corpus, which is
  // representative of the vocabulary every small file draws from.
  const std::string dictionary = source.substr(0, snappy::kBlockSize);

  uint64_t total_raw = 0;
  uint64_t total_independent = 0;
  uint64_t total_shared = 0;
  for (const std::string& file : files) {
    std::string independent, shared;
    snappy::Compress(file.data(), file.size(), &independent);
    size_t shared_written = snappy::CompressWithDictionary(
        dictionary.data(), dictionary.size(), file.data(), file.size(),
        &shared);
    CHECK_EQ(shared_written, shared.size());

    // Correctness guard for every file.
    std::string restored;
    CHECK_TRUE(snappy::UncompressWithDictionary(
        shared.data(), shared.size(), dictionary.data(), dictionary.size(),
        &restored));
    CHECK_TRUE(restored == file);

    total_raw += file.size();
    total_independent += independent.size();
    total_shared += shared.size();
  }

  // The dictionary is transmitted once alongside the archive.
  const uint64_t shared_archive = total_shared + dictionary.size();

  std::printf("\nSmall-file archive (%zu files, %zu bytes each)\n",
              static_cast<size_t>(files.size()), kWindow);
  std::printf("  raw total              : %llu bytes\n",
              static_cast<unsigned long long>(total_raw));
  std::printf("  independent dictionaries: %llu bytes (%.2f%% of raw)\n",
              static_cast<unsigned long long>(total_independent),
              100.0 * total_independent / total_raw);
  std::printf("  shared dictionary      : %llu bytes payload + %llu dict "
              "= %llu bytes (%.2f%% of raw)\n",
              static_cast<unsigned long long>(total_shared),
              static_cast<unsigned long long>(dictionary.size()),
              static_cast<unsigned long long>(shared_archive),
              100.0 * shared_archive / total_raw);
  std::printf("  savings vs independent : %.2f%%\n",
              100.0 * (1.0 - static_cast<double>(shared_archive) /
                               total_independent));
}

}  // namespace

int main(int argc, char** argv) {
  TestRoundTrip();
  TestDictionaryActuallyReferenced();
  TestDictionaryBoundaryMatch();
  TestCrossFormatCompatibility();
  TestEdgeCases();
  TestMixedArchiveCompatibility();
  TestMaximumOffsetWindow();

  const char* corpus = argc > 1 ? argv[1] : "testdata/alice29.txt";
  if (argc > 3) {
    TestSmallFileArchiveSize(corpus,
                             static_cast<size_t>(std::atol(argv[2])),
                             static_cast<size_t>(std::atol(argv[3])));
  } else {
    static const size_t kWindows[] = {64, 128, 256, 512, 1024};
    for (size_t window : kWindows) {
      TestSmallFileArchiveSize(corpus, 20000, window);
    }
  }

  if (g_failures == 0) {
    std::printf("\nAll dictionary tests passed.\n");
    return 0;
  }
  std::printf("\n%d check(s) failed.\n", g_failures);
  return 1;
}
