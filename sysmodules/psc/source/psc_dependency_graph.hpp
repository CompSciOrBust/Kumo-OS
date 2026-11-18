/* Framework-independent validation of the registered power dependency graph. */
#pragma once
#include <cstddef>
#include <cstdint>

namespace kumo::horizon::psc::srv {

    template <typename Entry>
    bool DependsOn(const Entry &entry, std::uint32_t id) {
        for (std::size_t i = 0; i < entry.dependency_count; ++i) {
            if (entry.dependencies[i] == id) return true;
        }
        return false;
    }

    // A participant may be asked when every relevant pending participant has
    // finished. Deregistered and completed entries do not constrain the order.
    template <typename Entry, typename Pending>
    bool CanRequest(const Entry &entry, const Entry *entries, std::size_t count,
                    bool lower_power, Pending pending) {
        for (std::size_t i = 0; i < count; ++i) {
            const Entry &other = entries[i];
            if (&other == &entry || !pending(other)) continue;
            if (lower_power ? DependsOn(other, static_cast<std::uint32_t>(entry.id))
                            : DependsOn(entry, static_cast<std::uint32_t>(other.id))) {
                return false;
            }
        }
        return true;
    }

    // Entry must expose id, dependencies and dependency_count. The caller rejects
    // dependencies on an entry's own ID before invoking this function.
    template <std::size_t Capacity, typename Entry>
    bool IsDependencyGraphValid(const Entry *const *entries, std::size_t count, std::uint32_t max_levels) {
        if (count > Capacity || max_levels == 0) return false;
        std::uint32_t levels[Capacity] = {};
        for (std::size_t i = 0; i < count; ++i) levels[i] = 1;

        for (std::uint32_t round = 0; round <= max_levels; ++round) {
            bool changed = false;
            for (std::size_t i = 0; i < count; ++i) {
                std::uint32_t level = 1;
                for (std::size_t j = 0; j < count; ++j) {
                    if (i == j) continue;
                    if (DependsOn(*entries[i], static_cast<std::uint32_t>(entries[j]->id)) && levels[j] >= level) {
                        level = levels[j] + 1;
                    }
                }
                if (level > max_levels) return false;
                if (level != levels[i]) {
                    levels[i] = level;
                    changed = true;
                }
            }
            if (!changed) return true;
        }
        return false;
    }
}
