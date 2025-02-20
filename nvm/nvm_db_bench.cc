// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unordered_set>
#include <sstream>
#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/write_batch.h"
#include "port/port.h"
#include "util/crc32c.h"
#include "util/histogram.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/testutil.h"
#include "silkstore/silkstore_impl.h"

// Comma-separated list of operations to run in the specified order
//   Actual benchmarks:
//      fillseq       -- write N values in sequential key order in async mode
//      fillrandom    -- write N values in random key order in async mode
//      overwrite     -- overwrite N values in random key order in async mode
//      fillsync      -- write N/100 values in random key order in sync mode
//      fill100K      -- write N/1000 100K values in random order in async mode
//      deleteseq     -- delete N keys in sequential order
//      deleterandom  -- delete N keys in random order
//      readseq       -- read N times sequentially
//      readreverse   -- read N times in reverse order
//      readrandom    -- read N times in random order
//      readmissing   -- read N missing keys in random order
//      readhot       -- read N times in random order from 1% section of DB
//      seekrandom    -- N random seeks
//      open          -- cost of opening a DB
//      crc32c        -- repeated crc32c of 4K of data
//      acquireload   -- load N*1000 times
//   Meta operations:
//      compact     -- Compact the entire DB
//      stats       -- Print DB stats
//      sstables    -- Print sstable info
//      heapprofile -- Dump a heap profile (if supported by this port)
static const char* FLAGS_benchmarks =
    "fillrandom,"
    // "fillseq,"
    // "overwrite,"
    // "readrandomsmall,"
    "shortrange,"
    "readseq,"

    // "readrandomsmall," // Extra run to allow previous compactions to quiesce
    /*
     "fillseq,"
     "readrandom,"
     "readseq,"


    "fillrandom,"
     "readrandomsmall,"  // Extra run to allow previous compactions to quiesce
    "readseq,"
    "overwrite," */

    /* "fillrandom,"
      "readrandomsmall,"  // Extra run to allow previous compactions to quiesce
     "overwrite,"
     //"readrandom,"
     "readrandomsmall,"  // Extra run to allow previous compactions to quiesce
     "readseq,"
   //  "readreverse,"
     "compact,"
     "readrandom,"
     "readseq,"
     "readreverse,"
     "fill100K,"
     "crc32c,"
     "snappycomp,"
     "snappyuncomp,"
     "acquireload,"
    // "readreverse,"

    "compact,"
     "readrandom,"
     "readseq,"
     "readreverse,"
     "fill100K,"
     "crc32c,"
     "snappycomp,"
     "snappyuncomp,"
     "acquireload,"
     */
    ;

// Number of key/values to place in database
static int FLAGS_num = 300000000;

// Number of read operations to do.  If negative, do FLAGS_num reads.
static int FLAGS_reads = -1;

// Number of write operations to do.  If negative, do FLAGS_num reads.
static int FLAGS_table_size = -1;

// Number of concurrent threads to run.
static int FLAGS_threads = 1;

// Size of each value
static int FLAGS_value_size = 128;

// Arrange to generate values that shrink to this fraction of
// their original size after compression
static double FLAGS_compression_ratio = 1;

// Print histogram of operation timings
static bool FLAGS_histogram = false;

// Number of bytes to buffer in memtable before compacting
// (initialized to default value by "main")
static int FLAGS_write_buffer_size = 0;

// Number of bytes written to each file.
// (initialized to default value by "main")
static int FLAGS_max_file_size = 0;

// Approximate size of user data packed per block (before compression.
// (initialized to default value by "main")
static int FLAGS_block_size = 0;

// Number of bytes to use as a cache of uncompressed data.
// Negative means use default settings.
static int FLAGS_cache_size = -1;

// Maximum number of files to keep open at the same time (use default if == 0)
static int FLAGS_open_files = 0;

// Bloom filter bits per key.
// Negative means use default settings.
static int FLAGS_bloom_bits = 10;

// If true, do not destroy the existing database.  If you set this
// flag and also specify a benchmark that wants a fresh database, that
// benchmark will fail.
static bool FLAGS_use_existing_db = false;

// If true, reuse existing log/MANIFEST files when re-opening a database.
static bool FLAGS_reuse_logs = false;

// Use the db with the following name.
static const char* FLAGS_db = "./nvmsilkstore_benckmark";
static int FLAGS_leaf_max_num_miniruns = 7;
static int FLAGS_memtbl_to_L0_ratio = 30;
// Test db impl type: leveldb/silkstore
static const char* FLAGS_db_type = "silkstore";

// Mixed workload spec
static const char* FLAGS_mixed_wl_spec = nullptr;

static int FLAGS_num_ops_in_mixed_wl = 0;

static bool FLAGS_enable_leaf_read_opt = true;

static bool FLAGS_enable_memtable_bloom = false;

// Ratio of the capacity of the log and the dataset
static double FLAGS_log_dataset_ratio = 2.0;

namespace leveldb {

namespace {
leveldb::Env* g_env = nullptr;

// Helper for quickly generating random data.
class RandomGenerator {
 private:
  std::string data_;
  int pos_;

 public:
  RandomGenerator() {
    // We use a limited amount of data over and over again and ensure
    // that it is larger than the compression window (32KB), and also
    // large enough to serve all typical value sizes we want to write.
    Random rnd(301);
    std::string piece;
    while (data_.size() < 1048576) {
      // Add a short fragment that is as compressible as specified
      // by FLAGS_compression_ratio.
      test::CompressibleString(&rnd, FLAGS_compression_ratio, 100, &piece);
      data_.append(piece);
    }
    pos_ = 0;
  }

  Slice Generate(size_t len) {
    if (pos_ + len > data_.size()) {
      pos_ = 0;
      assert(len < data_.size());
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
  }
};

#if defined(__linux)
static Slice TrimSpace(Slice s) {
  size_t start = 0;
  while (start < s.size() && isspace(s[start])) {
    start++;
  }
  size_t limit = s.size();
  while (limit > start && isspace(s[limit - 1])) {
    limit--;
  }
  return Slice(s.data() + start, limit - start);
}
#endif

static void AppendWithSpace(std::string* str, Slice msg) {
  if (msg.empty()) return;
  if (!str->empty()) {
    str->push_back(' ');
  }
  str->append(msg.data(), msg.size());
}

class Stats {
 private:
  double start_;
  double finish_;
  double seconds_;
  int done_;
  int next_report_;
  int64_t bytes_;
  double last_op_finish_;
  Histogram hist_;
  std::string message_;
  bool report_current = false;
  double last_current_report = 0;
  int done_since_last_current_report = 0;

 public:
  Stats() { Start(); }

  void EnableReportCurrent() { report_current = true; }

  void Start() {
    next_report_ = 100;
    last_op_finish_ = start_;
    hist_.Clear();
    done_ = 0;
    bytes_ = 0;
    seconds_ = 0;
    start_ = g_env->NowMicros();
    finish_ = start_;
    message_.clear();
    printf("Start bench \n");
  }

  void Merge(const Stats& other) {
    hist_.Merge(other.hist_);
    done_ += other.done_;
    bytes_ += other.bytes_;
    seconds_ += other.seconds_;
    if (other.start_ < start_) start_ = other.start_;
    if (other.finish_ > finish_) finish_ = other.finish_;

    // Just keep the messages from one thread
    if (message_.empty()) message_ = other.message_;
  }

  void Stop() {
    printf("Stop \n");
    finish_ = g_env->NowMicros();
    seconds_ = (finish_ - start_) * 1e-6;
  }

  void AddMessage(Slice msg) { AppendWithSpace(&message_, msg); }

  void FinishedSingleOp() {
    if (FLAGS_histogram) {
      double now = g_env->NowMicros();
      double micros = now - last_op_finish_;
      hist_.Add(micros);
      if (micros > 20000) {
        fprintf(stderr, "long op: %.1f micros%30s\r", micros, "");
        fflush(stderr);
      }
      last_op_finish_ = now;
    }

    done_++;
    if (done_ >= next_report_) {
      if (next_report_ < 1000)
        next_report_ += 100;
      else if (next_report_ < 5000)
        next_report_ += 500;
      else if (next_report_ < 10000)
        next_report_ += 1000;
      else if (next_report_ < 50000)
        next_report_ += 5000;
      else if (next_report_ < 100000)
        next_report_ += 10000;
      else if (next_report_ < 500000)
        next_report_ += 50000;
      else
        next_report_ += 100000;
      fprintf(stderr, "... finished %d ops%30s\r", done_, "");
      fflush(stderr);
    }
    if (report_current) {
      ++done_since_last_current_report;
      double elapsed = (g_env->NowMicros() - last_current_report);
      if (elapsed > 5 * 1e6) {
        double latency = elapsed / done_since_last_current_report;
        fprintf(stdout, "%.2f\n", latency);
        done_since_last_current_report = 0;
        last_current_report = g_env->NowMicros();
      }
    }
  }

  void AddBytes(int64_t n) { bytes_ += n; }

  void Report(const Slice& name) {
    // Pretend at least one op was done in case we are running a benchmark
    // that does not call FinishedSingleOp().
    if (done_ < 1) done_ = 1;

    std::string extra;
    if (bytes_ > 0) {
      // Rate is computed on actual elapsed time, not the sum of per-thread
      // elapsed times.
      double elapsed = (finish_ - start_) * 1e-6;
      char rate[100];
      snprintf(rate, sizeof(rate), "%6.1f MB/s",
               (bytes_ / 1048576.0) / elapsed);
      extra = rate;
    }
    AppendWithSpace(&extra, message_);

    fprintf(stdout, "%-12s : %11.3f micros/op;%s%s\n", name.ToString().c_str(),
            seconds_ * 1e6 / done_, (extra.empty() ? "" : " "), extra.c_str());
    if (FLAGS_histogram) {
      fprintf(stdout, "Microseconds per op:\n%s\n", hist_.ToString().c_str());
    }
    fflush(stdout);
  }
};

// State shared by all concurrent executions of the same benchmark.
struct SharedState {
  port::Mutex mu;
  port::CondVar cv GUARDED_BY(mu);
  int total GUARDED_BY(mu);

  // Each thread goes through the following states:
  //    (1) initializing
  //    (2) waiting for others to be initialized
  //    (3) running
  //    (4) done

  int num_initialized GUARDED_BY(mu);
  int num_done GUARDED_BY(mu);
  bool start GUARDED_BY(mu);

  explicit SharedState(int total)
      : cv(&mu), total(total), num_initialized(0), num_done(0), start(false) {}
};

// Per-thread state for concurrent executions of the same benchmark.
struct ThreadState {
  int tid;      // 0..n-1 when running in n threads
  Random rand;  // Has different seeds for different threads
  Stats stats;
  SharedState* shared;

  explicit ThreadState(int index) : tid(index), rand(1000 + index) {}
};

}  // namespace

class Workload {
 public:
  Workload(int tid, int weight) : tid(tid), weight(weight) {}
  virtual void work(ThreadState* thread) = 0;
  virtual void fillone(ThreadState* thread) = 0;
  virtual ~Workload() {}
  virtual size_t size() const = 0;
  int Weight() { return weight; }

 protected:
  int tid;
  int weight;
};

class ReadWriteWorkload : public Workload {
 public:
  ReadWriteWorkload(DB* db, int tid, int table_size, int write_ratio_in_percent,
                    int table_weight)
      : Workload(tid, table_weight),
        db_(db),
        table_size(table_size),
        write_ratio_in_percent(write_ratio_in_percent),
        value_size_(FLAGS_value_size) {}

  void work(ThreadState* thread) override {
    const int k = thread->rand.Next() % table_size;
    snprintf(key, sizeof(key), "%d.%016d", tid, k);
    if (thread->rand.Next() % 100 < write_ratio_in_percent) {
      batch.Clear();
      batch.Put(key, gen.Generate(value_size_));
      bytes += value_size_ + strlen(key);
      s = db_->Write(write_options_, &batch);
      if (!s.ok()) {
        fprintf(stderr, "put error: %s\n", s.ToString().c_str());
        exit(1);
      }
    } else {
      if (db_->Get(options, key, &value).ok()) {
        found++;
      }
    }

    thread->stats.FinishedSingleOp();
  }

  void fillone(ThreadState* thread) override {
    const int k = thread->rand.Next() % table_size;
    snprintf(key, sizeof(key), "%d.%016d", tid, k);
    batch.Clear();
    batch.Put(key, gen.Generate(value_size_));
    bytes += value_size_ + strlen(key);
    s = db_->Write(write_options_, &batch);
    if (!s.ok()) {
      fprintf(stderr, "put error: %s\n", s.ToString().c_str());
      exit(1);
    }
    thread->stats.FinishedSingleOp();
  }

  size_t size() const override { return table_size; }

 protected:
  DB* db_;
  char key[100];
  ReadOptions options;
  std::string value;
  int found = 0;
  int table_size;
  int write_ratio_in_percent;
  WriteBatch batch;
  RandomGenerator gen;
  size_t bytes = 0;
  size_t value_size_ = 0;
  Status s;
  WriteOptions write_options_;
};
//
// class ReadOnlyWorkload : public Workload {
// public:
//    ReadOnlyWorkload(DB* db, int table_size): rw(db, table_size, 0) {}
//
//    void work(ThreadState * thread, int tid) override {
//        rw.work(thread, tid);
//    }
// private:
//    ReadWriteWorkload rw;
//};
//
///**
// * 5% writes, 95% reads.
// */
// class ReadMostlyWorkload : public Workload {
// public:
//    ReadMostlyWorkload(DB* db, int table_size): rw(db, table_size, 5) {}
//
//    void work(ThreadState * thread, int tid) override {
//        rw.work(thread, tid);
//    }
// private:
//    ReadWriteWorkload rw;
//};
///**
// * 100% writes
// */
// class WriteOnlyWorkload : public Workload {
// public:
//    WriteOnlyWorkload(DB* db, int table_size): rw(db, table_size, 100) {}
//
//    void work(ThreadState * thread, int tid) override {
//        rw.work(thread, tid);
//    }
// private:
//    ReadWriteWorkload rw;
//};

class WorkloadSelector {
 public:
  explicit WorkloadSelector(const std::vector<Workload*> workloads)
      : workloads(workloads) {}
  virtual ~WorkloadSelector() {}
  virtual int Select(ThreadState* thread) = 0;

 protected:
  std::vector<Workload*> workloads;
};

class RandomWorkloadSelector : public WorkloadSelector {
 public:
  explicit RandomWorkloadSelector(const std::vector<Workload*> workloads)
      : WorkloadSelector(workloads) {}

  int Select(ThreadState* thread) override {
    return thread->rand.Next() % workloads.size();
  }
};

class WeightedRandomWorkloadSelector : public WorkloadSelector {
 public:
  explicit WeightedRandomWorkloadSelector(
      const std::vector<Workload*> workloads)
      : WorkloadSelector(workloads) {
    for (int i = 0; i < workloads.size(); ++i) {
      int weight = workloads[i]->Weight();
      for (int j = 0; j < weight; ++j) dice.push_back(i);
    }
    std::random_shuffle(dice.begin(), dice.end());
  }

  int Select(ThreadState* thread) override {
    return dice[thread->rand.Next() % dice.size()];
  }

 private:
  std::vector<int> dice;
};

// workload mixture spec syntax:
// (${write_ratio}-${table_size}-${weight};) *
//
class WorkloadMixture {
 private:
  std::vector<Workload*> workloads;
  WorkloadSelector* selector;

 public:
  WorkloadMixture(const std::vector<Workload*> workloads,
                  WorkloadSelector* selector)
      : workloads(workloads), selector(selector) {}

  void work(ThreadState* thread) {
    int wid = selector->Select(thread);
    workloads[wid]->work(thread);
  }

  void fill(ThreadState* thread) {
    int wid = selector->Select(thread);
    workloads[wid]->fillone(thread);
  }

  static std::vector<std::string> Split(const std::string& s, char delim) {
    std::stringstream ss(s);
    std::string item;
    std::vector<std::string> elems;
    while (std::getline(ss, item, delim)) {
      elems.push_back(
          std::move(item));  // if C++11 (based on comment from @mchiasson)
    }
    return elems;
  }

  static WorkloadMixture* ParseFromWorkloadSpec(DB* db,
                                                const std::string& spec) {
    std::vector<Workload*> workloads;
    auto parts = Split(spec, ';');
    for (int i = 0; i < parts.size(); ++i) {
      std::string p = parts[i];
      if (p.empty()) continue;
      auto wparts = Split(p, '-');
      assert(wparts.size() == 3);
      int write_ratio = std::stoi(wparts[0]);
      int table_size = std::stoi(wparts[1]);
      int table_weight = std::stoi(wparts[2]);
      int tid = i;
      ReadWriteWorkload* rw =
          new ReadWriteWorkload(db, tid, table_size, write_ratio, table_weight);
      workloads.push_back(rw);
    }
    return new WorkloadMixture(workloads,
                               new WeightedRandomWorkloadSelector(workloads));
  }
  size_t Size() const {
    size_t s = 0;
    for (size_t i = 0; i < workloads.size(); ++i) {
      s += workloads[i]->size();
    }
    return s;
  }
};

class Benchmark {
 private:
  Cache* cache_;
  const FilterPolicy* filter_policy_;
  DB* db_;
  int num_;
  int value_size_;
  int entries_per_batch_;
  WriteOptions write_options_;
  int reads_;
  int writes_;
  int heap_counter_;

  void PrintHeader() {
    const int kKeySize = 16;
    PrintEnvironment();
    fprintf(stdout, "Keys:       %d bytes each\n", kKeySize);
    fprintf(stdout, "Values:     %d bytes each (%d bytes after compression)\n",
            FLAGS_value_size,
            static_cast<int>(FLAGS_value_size * FLAGS_compression_ratio + 0.5));
    fprintf(stdout, "Entries:    %d\n", FLAGS_table_size);
    fprintf(stdout, "Reads:      %d\n", FLAGS_reads);
    fprintf(stdout, "Num:        %d\n", FLAGS_num);
    fprintf(stdout, "Leaf_max_num_miniruns:  %d\n",
            FLAGS_leaf_max_num_miniruns);
    fprintf(stdout, "Memtbl_to_L0_ratio:     %d\n", FLAGS_memtbl_to_L0_ratio);

    fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
            ((static_cast<int64_t>(kKeySize + FLAGS_value_size) *
              FLAGS_table_size) /
             1048576.0));
    fprintf(stdout, "FileSize:   %.1f MB (estimated)\n",
            (((kKeySize + FLAGS_value_size * FLAGS_compression_ratio) *
              FLAGS_table_size) /
             1048576.0));
    fprintf(stdout, "DBImplType:  %s\n", FLAGS_db_type);
    fprintf(stdout, "LogRatio:  %f\n", FLAGS_log_dataset_ratio);
    fprintf(stdout, "DBPath: %s\n", FLAGS_db);
    PrintWarnings();
    fprintf(stdout, "------------------------------------------------\n");
  }

  void PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    fprintf(
        stdout,
        "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n");
#endif
#ifndef NDEBUG
    fprintf(stdout,
            "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif

    // See if snappy is working by attempting to compress a compressible string
    const char text[] = "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy";
    std::string compressed;
    if (!port::Snappy_Compress(text, sizeof(text), &compressed)) {
      fprintf(stdout, "WARNING: Snappy compression is not enabled\n");
    } else if (compressed.size() >= sizeof(text)) {
      fprintf(stdout, "WARNING: Snappy compression is not effective\n");
    }
  }

  void PrintEnvironment() {
    fprintf(stderr, "LevelDB:    version %d.%d\n", kMajorVersion,
            kMinorVersion);

#if defined(__linux)
    time_t now = time(nullptr);
    fprintf(stderr, "Date:       %s", ctime(&now));  // ctime() adds newline

    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != nullptr) {
      char line[1000];
      int num_cpus = 0;
      std::string cpu_type;
      std::string cache_size;
      while (fgets(line, sizeof(line), cpuinfo) != nullptr) {
        const char* sep = strchr(line, ':');
        if (sep == nullptr) {
          continue;
        }
        Slice key = TrimSpace(Slice(line, sep - 1 - line));
        Slice val = TrimSpace(Slice(sep + 1));
        if (key == "model name") {
          ++num_cpus;
          cpu_type = val.ToString();
        } else if (key == "cache size") {
          cache_size = val.ToString();
        }
      }
      fclose(cpuinfo);
      fprintf(stderr, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
      fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
    }
#endif
  }

 public:
  Benchmark()
      : cache_(FLAGS_cache_size >= 0 ? NewLRUCache(FLAGS_cache_size) : nullptr),
        filter_policy_(FLAGS_bloom_bits >= 0
                           ? NewBloomFilterPolicy(FLAGS_bloom_bits)
                           : nullptr),
        db_(nullptr),
        num_(FLAGS_num),
        value_size_(FLAGS_value_size),
        entries_per_batch_(1),
        reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
        heap_counter_(0) {
    std::vector<std::string> files;
    g_env->GetChildren(FLAGS_db, &files);
    for (size_t i = 0; i < files.size(); i++) {
      if (Slice(files[i]).starts_with("heap-")) {
        g_env->DeleteFile(std::string(FLAGS_db) + "/" + files[i]);
      }
    }
    if (!FLAGS_use_existing_db) {
      Status s;
      if (FLAGS_db_type == std::string("silkstore")) {
        s = leveldb::silkstore::DestroyDB(FLAGS_db, Options());
      } else {
        s = leveldb::DestroyDB(FLAGS_db, Options());
      }
      if (!s.ok())
        fprintf(stderr, "DestroyDB failed: %s\n", s.ToString().c_str());
    }
  }

  ~Benchmark() {
    delete db_;
    delete cache_;
    delete filter_policy_;
  }

  void Run() {
    PrintHeader();
    Open();

    const char* benchmarks = FLAGS_benchmarks;
    while (benchmarks != nullptr) {
      const char* sep = strchr(benchmarks, ',');
      Slice name;
      if (sep == nullptr) {
        name = benchmarks;
        benchmarks = nullptr;
      } else {
        name = Slice(benchmarks, sep - benchmarks);
        benchmarks = sep + 1;
      }

      // Reset parameters that may be overridden below
      num_ = FLAGS_num;
      reads_ = (FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads);
      value_size_ = FLAGS_value_size;
      entries_per_batch_ = 1;
      write_options_ = WriteOptions();

      void (Benchmark::*method)(ThreadState*) = nullptr;
      bool fresh_db = false;
      int num_threads = FLAGS_threads;

      if (name == Slice("open")) {
        method = &Benchmark::OpenBench;
        num_ /= 10000;
        if (num_ < 1) num_ = 1;
      } else if (name == Slice("fillseq")) {
        fresh_db = true;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("fillbatch")) {
        fresh_db = true;
        entries_per_batch_ = 1000;
        method = &Benchmark::WriteSeq;
      } else if (name == Slice("fillrandom")) {
        fresh_db = true;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("writeskewed")) {
        fresh_db = true;
        method = &Benchmark::WriteSkewed;
      } else if (name == Slice("overwrite")) {
        fresh_db = false;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("fillsync")) {
        fresh_db = true;
        num_ /= 1000;
        write_options_.sync = true;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("fill100K")) {
        fresh_db = true;
        num_ /= 1000;
        value_size_ = 100 * 1000;
        method = &Benchmark::WriteRandom;
      } else if (name == Slice("readseq")) {
        method = &Benchmark::ReadSequential;
      } else if (name == Slice("shortrange")) {
        method = &Benchmark::ShortRangeQuery;
      } else if (name == Slice("readreverse")) {
        method = &Benchmark::ReadReverse;
      } else if (name == Slice("readrandom")) {
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("readmissing")) {
        method = &Benchmark::ReadMissing;
      } else if (name == Slice("seekrandom")) {
        method = &Benchmark::SeekRandom;
      } else if (name == Slice("readhot")) {
        method = &Benchmark::ReadHot;
      } else if (name == Slice("readrandomsmall")) {
        reads_ /= 1000;
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("deleteseq")) {
        method = &Benchmark::DeleteSeq;
      } else if (name == Slice("deleterandom")) {
        method = &Benchmark::DeleteRandom;
      } else if (name == Slice("readwhilewriting")) {
        num_threads++;  // Add extra thread for writing
        method = &Benchmark::ReadWhileWriting;
      } else if (name == Slice("compact")) {
        method = &Benchmark::Compact;
      } else if (name == Slice("crc32c")) {
        method = &Benchmark::Crc32c;
      } else if (name == Slice("acquireload")) {
        method = &Benchmark::AcquireLoad;
      } else if (name == Slice("snappycomp")) {
        method = &Benchmark::SnappyCompress;
      } else if (name == Slice("snappyuncomp")) {
        method = &Benchmark::SnappyUncompress;
      } else if (name == Slice("heapprofile")) {
        HeapProfile();
      } else if (name == Slice("stats")) {
        PrintStats("leveldb.stats");
      } else if (name == Slice("sstables")) {
        PrintStats("leveldb.sstables");
      } else if (name == Slice("mixed_workload")) {
        fresh_db = false;
        method = &Benchmark::MixedWorkload;
      } else if (name == Slice("mixed_workload_fillrandom")) {
        fresh_db = true;
        method = &Benchmark::MixedWorkloadFillRandom;
      } else {
        if (name != Slice()) {  // No error message for empty name
          fprintf(stderr, "unknown benchmark '%s'\n", name.ToString().c_str());
        }
      }

      if (fresh_db) {
        if (FLAGS_use_existing_db) {
          fprintf(stdout, "%-12s : skipped (--use_existing_db is true)\n",
                  name.ToString().c_str());
          method = nullptr;
        } else {
          delete db_;
          db_ = nullptr;
          if (FLAGS_db_type == std::string("silkstore")) {
            leveldb::silkstore::DestroyDB(FLAGS_db, Options());
          } else {
            leveldb::DestroyDB(FLAGS_db, Options());
          }
          Open();
        }
      }

      if (method != nullptr) {
        RunBenchmark(num_threads, name, method);
      }
    }
  }

 private:
  struct ThreadArg {
    Benchmark* bm;
    SharedState* shared;
    ThreadState* thread;
    void (Benchmark::*method)(ThreadState*);
  };

  static void ThreadBody(void* v) {
    ThreadArg* arg = reinterpret_cast<ThreadArg*>(v);
    SharedState* shared = arg->shared;
    ThreadState* thread = arg->thread;
    {
      MutexLock l(&shared->mu);
      shared->num_initialized++;
      if (shared->num_initialized >= shared->total) {
        shared->cv.SignalAll();
      }
      while (!shared->start) {
        shared->cv.Wait();
      }
    }

    thread->stats.Start();
    (arg->bm->*(arg->method))(thread);
    thread->stats.Stop();

    {
      MutexLock l(&shared->mu);
      shared->num_done++;
      if (shared->num_done >= shared->total) {
        shared->cv.SignalAll();
      }
    }
  }

  void RunBenchmark(int n, Slice name,
                    void (Benchmark::*method)(ThreadState*)) {
    SharedState shared(n);

    ThreadArg* arg = new ThreadArg[n];
    for (int i = 0; i < n; i++) {
      arg[i].bm = this;
      arg[i].method = method;
      arg[i].shared = &shared;
      arg[i].thread = new ThreadState(i);
      arg[i].thread->shared = &shared;
      g_env->StartThread(ThreadBody, &arg[i]);
    }

    shared.mu.Lock();
    while (shared.num_initialized < n) {
      shared.cv.Wait();
    }

    shared.start = true;
    shared.cv.SignalAll();
    while (shared.num_done < n) {
      shared.cv.Wait();
    }
    shared.mu.Unlock();

    for (int i = 1; i < n; i++) {
      arg[0].thread->stats.Merge(arg[i].thread->stats);
    }
    arg[0].thread->stats.Report(name);

    for (int i = 0; i < n; i++) {
      delete arg[i].thread;
    }
    delete[] arg;
  }

  void Crc32c(ThreadState* thread) {
    // Checksum about 500MB of data total
    const int size = 4096;
    const char* label = "(4K per op)";
    std::string data(size, 'x');
    int64_t bytes = 0;
    uint32_t crc = 0;
    while (bytes < 500 * 1048576) {
      crc = crc32c::Value(data.data(), size);
      thread->stats.FinishedSingleOp();
      bytes += size;
    }
    // Print so result is not dead
    fprintf(stderr, "... crc=0x%x\r", static_cast<unsigned int>(crc));

    thread->stats.AddBytes(bytes);
    thread->stats.AddMessage(label);
  }

  void AcquireLoad(ThreadState* thread) {
    int dummy;
    port::AtomicPointer ap(&dummy);
    int count = 0;
    void* ptr = nullptr;
    thread->stats.AddMessage("(each op is 1000 loads)");
    while (count < 100000) {
      for (int i = 0; i < 1000; i++) {
        ptr = ap.Acquire_Load();
      }
      count++;
      thread->stats.FinishedSingleOp();
    }
    if (ptr == nullptr) exit(1);  // Disable unused variable warning.
  }

  void SnappyCompress(ThreadState* thread) {
    RandomGenerator gen;
    Slice input = gen.Generate(Options().block_size);
    int64_t bytes = 0;
    int64_t produced = 0;
    bool ok = true;
    std::string compressed;
    while (ok && bytes < 1024 * 1048576) {  // Compress 1G
      ok = port::Snappy_Compress(input.data(), input.size(), &compressed);
      produced += compressed.size();
      bytes += input.size();
      thread->stats.FinishedSingleOp();
    }

    if (!ok) {
      thread->stats.AddMessage("(snappy failure)");
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "(output: %.1f%%)",
               (produced * 100.0) / bytes);
      thread->stats.AddMessage(buf);
      thread->stats.AddBytes(bytes);
    }
  }

  void SnappyUncompress(ThreadState* thread) {
    RandomGenerator gen;
    Slice input = gen.Generate(Options().block_size);
    std::string compressed;
    bool ok = port::Snappy_Compress(input.data(), input.size(), &compressed);
    int64_t bytes = 0;
    char* uncompressed = new char[input.size()];
    while (ok && bytes < 1024 * 1048576) {  // Compress 1G
      ok = port::Snappy_Uncompress(compressed.data(), compressed.size(),
                                   uncompressed);
      bytes += input.size();
      thread->stats.FinishedSingleOp();
    }
    delete[] uncompressed;

    if (!ok) {
      thread->stats.AddMessage("(snappy failure)");
    } else {
      thread->stats.AddBytes(bytes);
    }
  }

  void Open() {
    const int kKeySize = 16;
    assert(db_ == nullptr);
    Options options;
    options.env = g_env;
    options.create_if_missing = !FLAGS_use_existing_db;
    options.block_cache = cache_;
    options.nvmemtable_file = "/mnt/NVMSilkstore/nvmemtable";
    options.leaf_max_num_miniruns = FLAGS_leaf_max_num_miniruns;
    options.memtbl_to_L0_ratio = FLAGS_memtbl_to_L0_ratio;
    options.write_buffer_size = FLAGS_write_buffer_size;
    options.max_file_size = FLAGS_max_file_size;
    options.block_size = FLAGS_block_size;
    options.max_open_files = FLAGS_open_files;
    options.filter_policy = filter_policy_;
    options.reuse_logs = FLAGS_reuse_logs;
    options.compression = kNoCompression;
    options.enable_leaf_read_opt = FLAGS_enable_leaf_read_opt;
    options.use_memtable_dynamic_filter = FLAGS_enable_memtable_bloom;
    // options.leaf_index_path = "/mnt/myPMem";
    options.maximum_segments_storage_size =
        (static_cast<int64_t>(kKeySize + FLAGS_value_size) * FLAGS_table_size) *
        FLAGS_log_dataset_ratio;
    fprintf(stderr, "maximum_segments_storage_size %lu bytes\n",
            options.maximum_segments_storage_size);

    Status s;
    if (FLAGS_db_type == std::string("silkstore")) {
      DB::OpenSilkStore(options, FLAGS_db, &db_);
    } else {
      DB::Open(options, FLAGS_db, &db_);
    }
    if (!s.ok()) {
      fprintf(stderr, "open error: %s\n", s.ToString().c_str());
      exit(1);
    }
  }

  void OpenBench(ThreadState* thread) {
    for (int i = 0; i < num_; i++) {
      delete db_;
      Open();
      thread->stats.FinishedSingleOp();
    }
  }

  void WriteSeq(ThreadState* thread) { DoWrite(thread, true); }

  void WriteRandom(ThreadState* thread) { DoWrite(thread, false); }

  void DoWrite(ThreadState* thread, bool seq) {
    if (num_ != FLAGS_num) {
      char msg[100];
      snprintf(msg, sizeof(msg), "(%d ops)", num_);
      thread->stats.AddMessage(msg);
    }
    std::string msg;
    // db_->GetProperty(std::string(FLAGS_db_type) + ".stats", &msg);
    // thread->stats.AddMessage(msg);
    //  printf("###### NvmWriteBatch ######\n");
    RandomGenerator gen;
    //  NvmWriteBatch batch;
    WriteBatch batch;
    Status s;
    int64_t bytes = 0;
    for (int i = 0; i < num_; i += entries_per_batch_) {
      batch.Clear();
      for (int j = 0; j < entries_per_batch_; j++) {
        const int k = seq ? (i + j) % FLAGS_table_size
                          : (thread->rand.Next() % FLAGS_table_size);
        char key[100];
        snprintf(key, sizeof(key), "%016d", k);
        batch.Put(key, gen.Generate(value_size_));
        bytes += value_size_ + strlen(key);
        thread->stats.FinishedSingleOp();
      }
      //  s = db_->NvmWrite(write_options_, &batch);
      s = db_->Write(write_options_, &batch);
      if (!s.ok()) {
        fprintf(stderr, "put error: %s\n", s.ToString().c_str());
        exit(1);
      }
    }
    thread->stats.AddBytes(bytes);
    db_->GetProperty(std::string(FLAGS_db_type) + ".stats", &msg);
    thread->stats.AddMessage(msg);
    std::string time_spent_gc;
    db_->GetProperty("silkstore.gcstat", &time_spent_gc);
    thread->stats.AddMessage(time_spent_gc);
    std::string segment_util;
    db_->GetProperty("silkstore.segment_util", &segment_util);
    thread->stats.AddMessage(segment_util);
    std::string write_volume;
    db_->GetProperty(std::string(FLAGS_db_type) + ".write_volume",
                     &write_volume);
    std::string wm = "Write Amplification Factor: " +
                     std::to_string(std::stol(write_volume) / (bytes + 1.0));
    thread->stats.AddMessage(wm);
  }

  void WriteSkewed(ThreadState* thread) {
    RandomGenerator gen;
    WriteBatch batch;
    Status s;
    std::string msg;
    int64_t bytes = 0;
    // std::unordered_set<std::string> m;
    int max_log = ceil(std::log(FLAGS_table_size) / std::log(2));
    for (int i = 0; i < num_; i += entries_per_batch_) {
      batch.Clear();
      for (int j = 0; j < entries_per_batch_; j++) {
        const int k = thread->rand.Skewed(max_log);
        char key[100];
        snprintf(key, sizeof(key), "%016d", k);
        batch.Put(key, gen.Generate(value_size_));
        // m.insert(key);
        bytes += value_size_ + strlen(key);
        thread->stats.FinishedSingleOp();
      }
      s = db_->Write(write_options_, &batch);
      if (!s.ok()) {
        fprintf(stderr, "put error: %s\n", s.ToString().c_str());
        exit(1);
      }
    }

    thread->stats.AddBytes(bytes);
    // std::string num_unique_keys = std::to_string(m.size());
    // thread->stats.AddMessage(num_unique_keys + " unique keys ");
    db_->GetProperty(std::string(FLAGS_db_type) + ".stats", &msg);
    thread->stats.AddMessage(msg);
    std::string time_spent_gc;
    db_->GetProperty("silkstore.gcstat", &time_spent_gc);
    thread->stats.AddMessage(time_spent_gc);
    std::string segment_util;
    db_->GetProperty("silkstore.segment_util", &segment_util);
    thread->stats.AddMessage(segment_util);
  }

  void ShortRangeQuery(ThreadState* thread) {
    Iterator* iter = db_->NewIterator(ReadOptions());
    int64_t bytes = 0;
    int query_nums = 10000;
    int query_lens = 10000;
    int kv_nums = 0;
    for (int j = 0; j < query_nums; j++) {
      char key[100];
      const int k = thread->rand.Next() % FLAGS_table_size;
      snprintf(key, sizeof(key), "%016d", k);
      int i = 0;
      for (iter->Seek(key); i < query_lens && iter->Valid(); iter->Next()) {
        bytes += iter->key().size() + iter->value().size();
        thread->stats.FinishedSingleOp();
        ++i;
        kv_nums++;
      }
    }

    printf("Total reads_ %d  avil kv num's : %d \n", reads_, kv_nums);

    delete iter;
    char msg[1000];

    std::string runs_searched;
    db_->GetProperty("silkstore.runs_searched", &runs_searched);
    std::string leaf_avg_num_runs;
    db_->GetProperty("silkstore.leaf_avg_num_runs", &leaf_avg_num_runs);
    std::string num_leaves;
    db_->GetProperty("silkstore.num_leaves", &num_leaves);
    snprintf(msg, sizeof(msg),
             "(%d of %d found), runs_searched %s leaf_avg_num_runs %s "
             "num_leaves %s ",
             kv_nums, reads_, runs_searched.c_str(), leaf_avg_num_runs.c_str(),
             num_leaves.c_str());
    thread->stats.AddMessage(msg);
    thread->stats.AddBytes(bytes);
  }
  void ReadSequential(ThreadState* thread) {
    Iterator* iter = db_->NewIterator(ReadOptions());
    int i = 0;
    int64_t bytes = 0;
    for (iter->SeekToFirst(); i < reads_ && iter->Valid(); iter->Next()) {
      bytes += iter->key().size() + iter->value().size();
      thread->stats.FinishedSingleOp();
      ++i;
    }
    printf("Total reads_ %d  avil kv num's : %d \n", reads_, i);

    delete iter;
    char msg[100];
    snprintf(msg, sizeof(msg), "%ld bytes %d reads", bytes, i);
    thread->stats.AddMessage(msg);
    thread->stats.AddBytes(bytes);
  }

  void ReadReverse(ThreadState* thread) {
    Iterator* iter = db_->NewIterator(ReadOptions());
    int i = 0;
    int64_t bytes = 0;
    for (iter->SeekToLast(); i < reads_ && iter->Valid(); iter->Prev()) {
      bytes += iter->key().size() + iter->value().size();
      thread->stats.FinishedSingleOp();
      ++i;
    }
    delete iter;
    thread->stats.AddBytes(bytes);
  }

  void ReadRandom(ThreadState* thread) {
    ReadOptions options;
    std::string value;
    int found = 0;
    char msg[1000];
    std::string num_leaves;
    db_->GetProperty("silkstore.num_leaves", &num_leaves);
    snprintf(msg, sizeof(msg), "num_leaves %s", num_leaves.c_str());
    thread->stats.AddMessage(msg);
    for (int i = 0; i < reads_; i++) {
      char key[100];
      const int k = thread->rand.Next() % FLAGS_table_size;
      snprintf(key, sizeof(key), "%016d", k);
      if (db_->Get(options, key, &value).ok()) {
        found++;
      }
      thread->stats.FinishedSingleOp();
    }

    std::string runs_searched;
    db_->GetProperty("silkstore.runs_searched", &runs_searched);
    std::string leaf_avg_num_runs;
    db_->GetProperty("silkstore.leaf_avg_num_runs", &leaf_avg_num_runs);
    std::string searches_in_memtable;
    db_->GetProperty("silkstore.searches_in_memtable", &searches_in_memtable);
    snprintf(msg, sizeof(msg),
             "(%d of %d found), runs_searched %s leaf_avg_num_runs %s "
             "searches_in_memtable %s ",
             found, num_, runs_searched.c_str(), leaf_avg_num_runs.c_str(),
             searches_in_memtable.c_str());
    thread->stats.AddMessage(msg);

    // std::string leaf_stats;
    // db_->GetProperty("silkstore.leaf_stats", &leaf_stats);
    // thread->stats.AddMessage(leaf_stats);
  }

  void ReadMissing(ThreadState* thread) {
    ReadOptions options;
    std::string value;
    for (int i = 0; i < reads_; i++) {
      char key[100];
      const int k = thread->rand.Next() % FLAGS_table_size;
      snprintf(key, sizeof(key), "%016d.", k);
      db_->Get(options, key, &value);
      thread->stats.FinishedSingleOp();
    }
  }

  void ReadHot(ThreadState* thread) {
    ReadOptions options;
    std::string value;
    const int range = (FLAGS_table_size + 99) / 100;
    for (int i = 0; i < reads_; i++) {
      char key[100];
      const int k = thread->rand.Next() % range;
      snprintf(key, sizeof(key), "%016d", k);
      db_->Get(options, key, &value);
      thread->stats.FinishedSingleOp();
    }
  }

  void SeekRandom(ThreadState* thread) {
    ReadOptions options;
    int found = 0;
    for (int i = 0; i < reads_; i++) {
      Iterator* iter = db_->NewIterator(options);
      char key[100];
      const int k = thread->rand.Next() % FLAGS_table_size;
      snprintf(key, sizeof(key), "%016d", k);
      iter->Seek(key);
      if (iter->Valid() && iter->key() == key) found++;
      delete iter;
      thread->stats.FinishedSingleOp();
    }
    char msg[100];
    snprintf(msg, sizeof(msg), "(%d of %d found)", found, num_);
    thread->stats.AddMessage(msg);
  }

  void DoDelete(ThreadState* thread, bool seq) {
    RandomGenerator gen;
    WriteBatch batch;
    Status s;
    for (int i = 0; i < num_; i += entries_per_batch_) {
      batch.Clear();
      for (int j = 0; j < entries_per_batch_; j++) {
        const int k = seq ? (i + j) % FLAGS_table_size
                          : (thread->rand.Next() % FLAGS_table_size);
        char key[100];
        snprintf(key, sizeof(key), "%016d", k);
        batch.Delete(key);
        thread->stats.FinishedSingleOp();
      }
      s = db_->Write(write_options_, &batch);
      if (!s.ok()) {
        fprintf(stderr, "del error: %s\n", s.ToString().c_str());
        exit(1);
      }
    }
  }

  void DeleteSeq(ThreadState* thread) { DoDelete(thread, true); }

  void DeleteRandom(ThreadState* thread) { DoDelete(thread, false); }

  void ReadWhileWriting(ThreadState* thread) {
    if (thread->tid > 0) {
      ReadRandom(thread);
    } else {
      // Special thread that keeps writing until other threads are done.
      RandomGenerator gen;
      while (true) {
        {
          MutexLock l(&thread->shared->mu);
          if (thread->shared->num_done + 1 >= thread->shared->num_initialized) {
            // Other threads have finished
            break;
          }
        }

        const int k = thread->rand.Next() % FLAGS_table_size;
        char key[100];
        snprintf(key, sizeof(key), "%016d", k);
        Status s = db_->Put(write_options_, key, gen.Generate(value_size_));
        if (!s.ok()) {
          fprintf(stderr, "put error: %s\n", s.ToString().c_str());
          exit(1);
        }
      }

      // Do not count any of the preceding work/delay in stats.
      thread->stats.Start();
    }
  }

  void Compact(ThreadState* thread) { db_->CompactRange(nullptr, nullptr); }

  void PrintStats(const char* key) {
    std::string stats;
    if (!db_->GetProperty(key, &stats)) {
      stats = "(failed)";
    }
    fprintf(stdout, "\n%s\n", stats.c_str());
  }

  void MixedWorkload(ThreadState* thread) {
    WorkloadMixture* mixture =
        WorkloadMixture::ParseFromWorkloadSpec(db_, FLAGS_mixed_wl_spec);
    assert(mixture);
    char msg[1000];
    thread->stats.EnableReportCurrent();
    for (int i = 0; i < FLAGS_num_ops_in_mixed_wl; ++i) {
      mixture->work(thread);
    }
    std::string runs_searched;
    db_->GetProperty("silkstore.runs_searched", &runs_searched);
    std::string leaf_avg_num_runs;
    db_->GetProperty("silkstore.leaf_avg_num_runs", &leaf_avg_num_runs);
    std::string searches_in_memtable;
    db_->GetProperty("silkstore.searches_in_memtable", &searches_in_memtable);
    std::string num_leaves;
    db_->GetProperty("silkstore.num_leaves", &num_leaves);
    std::string segment_util;
    db_->GetProperty("silkstore.segment_util", &segment_util);
    std::string stats;
    db_->GetProperty(std::string(FLAGS_db_type) + ".stats", &stats);
    std::string time_spent_gc;
    db_->GetProperty("silkstore.gcstat", &time_spent_gc);
    snprintf(msg, sizeof(msg),
             "%d ops, runs_searched %s leaf_avg_num_runs %s "
             "searches_in_memtable %s num_leaves %s\n%s\n%s\n%s\n",
             FLAGS_num_ops_in_mixed_wl, runs_searched.c_str(),
             leaf_avg_num_runs.c_str(), searches_in_memtable.c_str(),
             num_leaves.c_str(), segment_util.c_str(), stats.c_str(),
             time_spent_gc.c_str());
    thread->stats.AddMessage(msg);
  }

  void MixedWorkloadFillRandom(ThreadState* thread) {
    WorkloadMixture* mixture =
        WorkloadMixture::ParseFromWorkloadSpec(db_, FLAGS_mixed_wl_spec);
    assert(mixture);
    size_t table_total_size = mixture->Size();
    fprintf(stderr, "table_total_size %lu\n", table_total_size);
    for (int i = 0; i < FLAGS_table_size; ++i) {
      mixture->fill(thread);
    }
    char msg[3000];
    std::string num_leaves;
    db_->GetProperty("silkstore.num_leaves", &num_leaves);
    std::string segment_util;
    db_->GetProperty("silkstore.segment_util", &segment_util);
    std::string stats;
    db_->GetProperty(std::string(FLAGS_db_type) + ".stats", &stats);
    std::string time_spent_gc;
    db_->GetProperty("silkstore.gcstat", &time_spent_gc);
    snprintf(msg, sizeof(msg), "num_leaves %s\n%s\n%s\n%s\n",
             num_leaves.c_str(), segment_util.c_str(), stats.c_str(),
             time_spent_gc.c_str());
    thread->stats.AddMessage(msg);
  }

  static void WriteToFile(void* arg, const char* buf, int n) {
    reinterpret_cast<WritableFile*>(arg)->Append(Slice(buf, n));
  }

  void HeapProfile() {
    char fname[100];
    snprintf(fname, sizeof(fname), "%s/heap-%04d", FLAGS_db, ++heap_counter_);
    WritableFile* file;
    Status s = g_env->NewWritableFile(fname, &file);
    if (!s.ok()) {
      fprintf(stderr, "%s\n", s.ToString().c_str());
      return;
    }
    bool ok = port::GetHeapProfile(WriteToFile, file);
    delete file;
    if (!ok) {
      fprintf(stderr, "heap profiling not supported\n");
      g_env->DeleteFile(fname);
    }
  }
};
}  // namespace leveldb

int main(int argc, char** argv) {
  FLAGS_write_buffer_size = leveldb::Options().write_buffer_size;
  FLAGS_max_file_size = leveldb::Options().max_file_size;
  FLAGS_block_size = leveldb::Options().block_size;
  FLAGS_open_files = leveldb::Options().max_open_files;
  std::string default_db_path;

  for (int i = 1; i < argc; i++) {
    double d;
    int n;
    char junk;
    if (leveldb::Slice(argv[i]).starts_with("--benchmarks=")) {
      FLAGS_benchmarks = argv[i] + strlen("--benchmarks=");
    } else if (sscanf(argv[i], "--compression_ratio=%lf%c", &d, &junk) == 1) {
      FLAGS_compression_ratio = d;
    } else if (sscanf(argv[i], "--histogram=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_histogram = n;
    } else if (sscanf(argv[i], "--use_existing_db=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_existing_db = n;
    } else if (sscanf(argv[i], "--reuse_logs=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_reuse_logs = n;
    } else if (sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
      FLAGS_num = n;
    } else if (sscanf(argv[i], "--reads=%d%c", &n, &junk) == 1) {
      FLAGS_reads = n;
    } else if (sscanf(argv[i], "--threads=%d%c", &n, &junk) == 1) {
      FLAGS_threads = n;
    } else if (sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
      FLAGS_value_size = n;
    } else if (sscanf(argv[i], "--write_buffer_size=%d%c", &n, &junk) == 1) {
      FLAGS_write_buffer_size = n;
    } else if (sscanf(argv[i], "--max_file_size=%d%c", &n, &junk) == 1) {
      FLAGS_max_file_size = n;
    } else if (sscanf(argv[i], "--block_size=%d%c", &n, &junk) == 1) {
      FLAGS_block_size = n;
    } else if (sscanf(argv[i], "--cache_size=%d%c", &n, &junk) == 1) {
      FLAGS_cache_size = n;
    } else if (sscanf(argv[i], "--bloom_bits=%d%c", &n, &junk) == 1) {
      FLAGS_bloom_bits = n;
    } else if (sscanf(argv[i], "--open_files=%d%c", &n, &junk) == 1) {
      FLAGS_open_files = n;
    } else if (strncmp(argv[i], "--db=", 5) == 0) {
      FLAGS_db = argv[i] + 5;
    } else if (strncmp(argv[i], "--db_type=", 10) == 0) {
      FLAGS_db_type = argv[i] + 10;
    } else if (strncmp(argv[i], "--mixed_wl_sepc=", 16) == 0) {
      FLAGS_mixed_wl_spec = argv[i] + 16;
    } else if (strncmp(argv[i], "--num_ops_in_mixed_wl=", 22) == 0) {
      FLAGS_num_ops_in_mixed_wl = std::stoi(argv[i] + 22);
    } else if (strncmp(argv[i], "--enable_leaf_read_opt=", 23) == 0) {
      FLAGS_enable_leaf_read_opt = std::stoi(argv[i] + 23);
    } else if (strncmp(argv[i], "--enable_memtable_bloom=", 24) == 0) {
      FLAGS_enable_memtable_bloom = std::stoi(argv[i] + 24);
    } else if (strncmp(argv[i], "--table_size=", 13) == 0) {
      FLAGS_table_size = std::stoi(argv[i] + 13);
    } else if (strncmp(argv[i], "--log_dataset_ratio=", 20) == 0) {
      FLAGS_log_dataset_ratio = std::stof(argv[i] + 20);
    } else {
      fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      exit(1);
    }
  }

  leveldb::g_env = leveldb::Env::Default();

  // Choose a location for the test database if none given with --db=<path>
  if (FLAGS_db == nullptr) {
    // leveldb::g_env->GetTestDirectory(&default_db_path);
    // default_db_path = "/mnt/900p/dbbench";
    default_db_path = "/mnt/toshiba/nvmbench";
    std::cout << "default_db_path: " << default_db_path << "\n";
    //      if (FLAGS_db_type == std::string("silkstore"))
    //          default_db_path += "/silkstore";
    //      else
    //          default_db_path += "/leveldb";
    FLAGS_db = default_db_path.c_str();
  }

  if (FLAGS_table_size == -1) FLAGS_table_size = FLAGS_num;
  leveldb::Benchmark benchmark;
  benchmark.Run();
  return 0;
}
