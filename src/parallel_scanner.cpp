#include "parallel_scanner.h"
#include "tsconfig.h"
#include "logger.h"

ParallelScanner::ParallelScanner(Store* store, Collection* collection,
                                 uint32_t max_seq_id, uint32_t num_threads,
                                 uint32_t batch_size)
    : store_(store), collection_(collection), max_seq_id_(max_seq_id),
      num_threads_(num_threads), batch_size_(batch_size) {}

ParallelScanner::~ParallelScanner() {
    quit_ = true;
    queue_not_full_.notify_all();
    for(auto& t : threads_) {
        if(t.joinable()) t.join();
    }
}

void ParallelScanner::start() {
    if(max_seq_id_ == 0) return;

    uint32_t range_size = (max_seq_id_ + num_threads_ - 1) / num_threads_;

    for(uint32_t i = 0; i < num_threads_; i++) {
        uint32_t range_start = i * range_size;
        uint32_t range_end = std::min((i + 1) * range_size, max_seq_id_);
        if(range_start >= max_seq_id_) break;

        active_scanners_++;
        threads_.emplace_back(&ParallelScanner::scanner_thread, this,
                              range_start, range_end);
    }
}

void ParallelScanner::scanner_thread(uint32_t range_start, uint32_t range_end) {
    const std::string seq_id_prefix = collection_->get_seq_id_collection_prefix();

    // Build RocksDB key for range_start
    std::string start_key = seq_id_prefix + "_"
        + Collection::get_seq_id_key_suffix(range_start);

    std::string end_key = seq_id_prefix + "_"
        + Collection::get_seq_id_key_suffix(range_end);
    rocksdb::Slice upper_bound(end_key);

    rocksdb::Iterator* iter = store_->scan(start_key, &upper_bound);
    std::unique_ptr<rocksdb::Iterator> iter_guard(iter);

    ScannedBatch current_batch;
    size_t num_valid = 0;

    while(iter->Valid() && iter->key().starts_with(seq_id_prefix) && !quit_) {
        // Parse seq_id from key
        uint32_t seq_id = Collection::get_seq_id_from_key(iter->key().ToString());
        if(seq_id >= range_end) break;

        const std::string doc_string = iter->value().ToString();
        current_batch.doc_str_size += doc_string.size();

        nlohmann::json document;
        try {
            document = nlohmann::json::parse(doc_string);
        } catch(const std::exception& e) {
            total_parse_errors++;
            iter->Next();
            continue;
        }

        if(collection_->get_enable_nested_fields()) {
            std::vector<field> flattened_fields;
            field::flatten_doc(document, collection_->get_nested_fields(),
                             {}, false, flattened_fields);
        }

        auto dirty_values = DIRTY_VALUES::COERCE_OR_DROP;
        current_batch.records.emplace_back(
            index_record(0, seq_id, document, CREATE, dirty_values));
        num_valid++;

        bool exceeds_mem = ((current_batch.doc_str_size * 7) > (250 * 1014 * 1024));
        bool batch_full = (num_valid % batch_size_ == 0);

        iter->Next();
        bool last_record = !(iter->Valid() && iter->key().starts_with(seq_id_prefix))
                          || seq_id + 1 >= range_end;

        if(exceeds_mem || batch_full || last_record) {
            if(!current_batch.records.empty()) {
                // Enqueue batch (blocks if queue full)
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_not_full_.wait(lock, [this]() {
                    return batch_queue_.size() < MAX_QUEUE_DEPTH || quit_;
                });
                if(quit_) break;

                total_docs_scanned += current_batch.records.size();
                batch_queue_.push(std::move(current_batch));
                queue_not_empty_.notify_one();
            }
            current_batch = ScannedBatch{};
            num_valid = 0;
        }
    }

    // Flush any remaining records
    if(!current_batch.records.empty()) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_not_full_.wait(lock, [this]() {
            return batch_queue_.size() < MAX_QUEUE_DEPTH || quit_;
        });
        total_docs_scanned += current_batch.records.size();
        batch_queue_.push(std::move(current_batch));
        queue_not_empty_.notify_one();
    }

    active_scanners_--;
    queue_not_empty_.notify_all();  // Wake consumer if all scanners done
}

bool ParallelScanner::next_batch(ScannedBatch& batch) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_not_empty_.wait(lock, [this]() {
        return !batch_queue_.empty() || active_scanners_ == 0;
    });

    if(batch_queue_.empty() && active_scanners_ == 0) {
        return false;  // All done
    }

    if(batch_queue_.empty()) {
        return false;
    }

    batch = std::move(batch_queue_.front());
    batch_queue_.pop();
    queue_not_full_.notify_one();
    return true;
}
