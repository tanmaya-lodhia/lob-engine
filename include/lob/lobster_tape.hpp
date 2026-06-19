#pragma once

#include <string>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// One "child order": a marketable order, i.e. a burst of executions sharing a
// timestamp and sign, reconstructed from a LOBSTER feed. pre_mid is the mid just
// before the burst; post_mid is the mid just after it (= the mid just before the
// next market order), as required by the metaorder impact definition.
struct Trade {
    double   time;
    int      sign;      // +1 buyer-initiated, -1 seller-initiated (eps = -Direction)
    Quantity volume;    // shares in the child order
    double   pre_mid;   // log price not taken here; raw mid in ticks
    double   post_mid;
};

// The day's trade tape plus the two normalizers used by the square-root law.
struct Tape {
    std::vector<Trade> trades;
    Quantity           total_volume = 0;     // V_D = sum of all executed volume
    double             sigma_d      = 0.0;   // (max mid - min mid) / first mid
};

// Reconstruct the book from message + orderbook files (seeding seed_levels deep
// from the first snapshot) and extract the trade tape. Returns false and sets
// `err` on failure.
bool load_tape(const std::string& message_file, const std::string& orderbook_file, int seed_levels,
               Tape& out, std::string& err);

}  // namespace lob
