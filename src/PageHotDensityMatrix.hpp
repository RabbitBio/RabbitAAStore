#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace page_hot_density {

class DensePageDemand {
public:
    explicit DensePageDemand(size_t page_count)
        : residues_(page_count, 0), refs_(page_count, 0) {}

    void add(uint64_t page_id, uint64_t residues) {
        const size_t index = page_index(page_id);
        if (residues_[index] == 0 && residues != 0) {
            touched_pages_.push_back(static_cast<uint32_t>(page_id));
        }
        residues_[index] += residues;
        ++refs_[index];
    }

    uint64_t residues(uint64_t page_id) const {
        return residues_[page_index(page_id)];
    }

    uint32_t refs(uint64_t page_id) const {
        return refs_[page_index(page_id)];
    }

    const std::vector<uint32_t>& touched_pages() const {
        return touched_pages_;
    }

    void clear_touched() {
        for (uint32_t page_id : touched_pages_) {
            residues_[page_id] = 0;
            refs_[page_id] = 0;
        }
        touched_pages_.clear();
    }

private:
    size_t page_index(uint64_t page_id) const {
        assert(page_id < residues_.size());
        return static_cast<size_t>(page_id);
    }

    std::vector<uint64_t> residues_;
    std::vector<uint32_t> refs_;
    std::vector<uint32_t> touched_pages_;
};

inline bool density_meets_percent(uint64_t demand_residues,
                                  uint64_t page_residues,
                                  uint32_t threshold_percent) {
    return page_residues != 0 &&
           static_cast<__uint128_t>(demand_residues) * 100 >=
               static_cast<__uint128_t>(page_residues) * threshold_percent;
}

struct DensityBuckets {
    uint64_t lt_25 = 0;
    uint64_t ge_25_lt_50 = 0;
    uint64_t ge_50_lt_75 = 0;
    uint64_t ge_75_lt_100 = 0;
    uint64_t ge_100_lt_200 = 0;
    uint64_t ge_200 = 0;
};

inline DensityBuckets classify_density(uint64_t demand_residues,
                                       uint64_t page_residues) {
    DensityBuckets buckets;
    if (!density_meets_percent(demand_residues, page_residues, 25)) {
        buckets.lt_25 = 1;
    } else if (!density_meets_percent(demand_residues, page_residues, 50)) {
        buckets.ge_25_lt_50 = 1;
    } else if (!density_meets_percent(demand_residues, page_residues, 75)) {
        buckets.ge_50_lt_75 = 1;
    } else if (!density_meets_percent(demand_residues, page_residues, 100)) {
        buckets.ge_75_lt_100 = 1;
    } else if (!density_meets_percent(demand_residues, page_residues, 200)) {
        buckets.ge_100_lt_200 = 1;
    } else {
        buckets.ge_200 = 1;
    }
    return buckets;
}

class TwoQTracker {
public:
    TwoQTracker(size_t page_count, uint16_t window_epochs)
        : first_seen_epoch_(page_count, 0),
          seen_(page_count, false),
          promoted_(page_count, false),
          window_epochs_(window_epochs) {}

    bool observe(uint64_t page_id, uint16_t epoch) {
        const size_t index = page_index(page_id);
        if (promoted_[index]) {
            return false;
        }
        if (!seen_[index]) {
            seen_[index] = true;
            first_seen_epoch_[index] = epoch;
            return false;
        }

        const uint16_t elapsed =
            static_cast<uint16_t>(epoch - first_seen_epoch_[index]);
        if (elapsed <= window_epochs_) {
            promoted_[index] = true;
            return true;
        }

        first_seen_epoch_[index] = epoch;
        return false;
    }

    bool promoted(uint64_t page_id) const {
        return promoted_[page_index(page_id)];
    }

private:
    size_t page_index(uint64_t page_id) const {
        assert(page_id < promoted_.size());
        return static_cast<size_t>(page_id);
    }

    std::vector<uint16_t> first_seen_epoch_;
    std::vector<bool> seen_;
    std::vector<bool> promoted_;
    uint16_t window_epochs_;
};

}  // namespace page_hot_density
