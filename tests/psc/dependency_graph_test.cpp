#include "../../sysmodules/psc/source/psc_dependency_graph.hpp"
#include <cassert>
#include <cstdint>

struct Entry {
    std::uint32_t id;
    std::uint32_t dependencies[2];
    std::size_t dependency_count;
    bool pending = true;
};

int main() {
    using kumo::horizon::psc::srv::IsDependencyGraphValid;
    Entry entries[21] = {};
    const Entry *graph[21];
    for (std::size_t i = 0; i < 21; ++i) {
        entries[i].id = static_cast<std::uint32_t>(i);
        graph[i] = &entries[i];
        if (i) {
            entries[i].dependencies[0] = static_cast<std::uint32_t>(i - 1);
            entries[i].dependency_count = 1;
        }
    }
    assert(IsDependencyGraphValid<21>(graph, 20, 20));
    assert(!IsDependencyGraphValid<21>(graph, 21, 20));
    entries[0].dependencies[0] = 19;
    entries[0].dependency_count = 1;
    assert(!IsDependencyGraphValid<21>(graph, 20, 20));
    entries[0].dependencies[0] = 999; // Missing dependencies are ignored.
    assert(IsDependencyGraphValid<21>(graph, 20, 20));
    // A dependency on a shared ID waits for all matching entries.
    Entry duplicate[] = {{1, {}, 0}, {1, {2}, 1}, {2, {1}, 1}};
    const Entry *duplicates[] = {&duplicate[0], &duplicate[1], &duplicate[2]};
    assert(!IsDependencyGraphValid<3>(duplicates, 3, 20));
    assert(!IsDependencyGraphValid<2>(duplicates, 3, 20));

    using kumo::horizon::psc::srv::CanRequest;
    Entry chain[] = {{1, {}, 0}, {2, {1}, 1}, {3, {2}, 1}, {4, {}, 0}};
    const auto pending = [](const Entry &entry) { return entry.pending; };
    assert(!CanRequest(chain[0], chain, 4, true, pending));
    assert(CanRequest(chain[2], chain, 4, true, pending));
    assert(CanRequest(chain[3], chain, 4, true, pending));
    chain[2].pending = false;
    assert(CanRequest(chain[1], chain, 4, true, pending));
    assert(!CanRequest(chain[1], chain, 4, false, pending));
    chain[0].pending = false;
    assert(CanRequest(chain[1], chain, 4, false, pending));
    Entry shared[] = {{1, {}, 0}, {1, {}, 0}, {2, {1}, 1}};
    shared[0].pending = false;
    assert(!CanRequest(shared[2], shared, 3, false, pending));
    shared[1].pending = false;
    assert(CanRequest(shared[2], shared, 3, false, pending));
}
