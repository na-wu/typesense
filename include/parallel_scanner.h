#pragma once
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include "store.h"
#include "collection.h"
#include "json.hpp"

struct ScannedBatch {
    std::vector<index_record> records;
    size_t doc_str_size = 0;
};

class ParallelScanner {
public:
    ParallelScanner(Store* store, Collection* collection,
                    uint32_t max_seq_id, uint32_t num_threads,
                    uint32_t batch_size, bool pre_tokenize = false);

    ~ParallelScanner();

    // Start scanning threads
    void start();

    // Get next batch (blocks until available or all scanners done)
    // Returns false when all batches have been consumed
    bool next_batch(ScannedBatch& batch);

    // Get total documents scanned
    size_t total_scanned() const { return total_docs_scanned.load(); }

    // Get total documents with parse errors
    size_t total_errors() const { return total_parse_errors.load(); }

private:
    void scanner_thread(uint32_t range_start, uint32_t range_end);

    Store* store_;
    Collection* collection_;
    uint32_t max_seq_id_;
    uint32_t num_threads_;
    uint32_t batch_size_;
    bool pre_tokenize_;

    // Thread-safe bounded batch queue
    std::queue<ScannedBatch> batch_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_not_empty_;
    std::condition_variable queue_not_full_;
    static constexpr size_t MAX_QUEUE_DEPTH = 8;  // limit memory usage

    std::atomic<uint32_t> active_scanners_{0};
    std::atomic<size_t> total_docs_scanned{0};
    std::atomic<size_t> total_parse_errors{0};

    std::vector<std::thread> threads_;
    std::atomic<bool> quit_{false};
};
