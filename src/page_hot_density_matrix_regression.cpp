#include "PageHotDensityMatrix.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

void test_dense_page_demand_accumulates_repeated_chunks() {
    page_hot_density::DensePageDemand demand(4);
    demand.add(1, 20);
    demand.add(1, 30);

    assert(demand.residues(1) == 50);
    assert(demand.refs(1) == 2);
    assert(demand.touched_pages().size() == 1);
    assert(demand.touched_pages().front() == 1);
}

void test_clear_touched_resets_only_tracked_pages() {
    page_hot_density::DensePageDemand demand(4);
    demand.add(0, 11);
    demand.add(3, 7);
    demand.clear_touched();

    assert(demand.residues(0) == 0);
    assert(demand.refs(0) == 0);
    assert(demand.residues(3) == 0);
    assert(demand.refs(3) == 0);
    assert(demand.touched_pages().empty());

    demand.add(2, 9);
    assert(demand.residues(2) == 9);
    assert(demand.refs(2) == 1);
    assert(demand.touched_pages().size() == 1);
    assert(demand.touched_pages().front() == 2);
}

void test_density_thresholds_at_full_and_tail_page_boundaries() {
    assert(page_hot_density::density_meets_percent(50, 100, 50));
    assert(!page_hot_density::density_meets_percent(49, 100, 50));
    assert(page_hot_density::density_meets_percent(20, 40, 50));
    assert(!page_hot_density::density_meets_percent(19, 40, 50));
}

void test_density_classifies_two_hundred_percent_bucket() {
    const page_hot_density::DensityBuckets buckets =
        page_hot_density::classify_density(200, 100);

    assert(buckets.lt_25 == 0);
    assert(buckets.ge_25_lt_50 == 0);
    assert(buckets.ge_50_lt_75 == 0);
    assert(buckets.ge_75_lt_100 == 0);
    assert(buckets.ge_100_lt_200 == 0);
    assert(buckets.ge_200 == 1);
}

void test_two_q_requires_second_access_within_window() {
    page_hot_density::TwoQTracker tracker(3, 8);

    assert(!tracker.observe(1, 10));
    assert(!tracker.promoted(1));
    assert(tracker.observe(1, 18));
    assert(tracker.promoted(1));
}

void test_two_q_does_not_promote_after_window() {
    page_hot_density::TwoQTracker tracker(3, 8);

    assert(!tracker.observe(2, 10));
    assert(!tracker.observe(2, 19));
    assert(!tracker.promoted(2));
}

}  // namespace

int main() {
    test_dense_page_demand_accumulates_repeated_chunks();
    test_clear_touched_resets_only_tracked_pages();
    test_density_thresholds_at_full_and_tail_page_boundaries();
    test_density_classifies_two_hundred_percent_bucket();
    test_two_q_requires_second_access_within_window();
    test_two_q_does_not_promote_after_window();
    std::cout << "page hot density regression: ok\n";
    return 0;
}
