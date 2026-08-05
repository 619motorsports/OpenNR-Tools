#pragma once

#include <cstdint>

namespace opennr {

// The garage Drivetrain panel stores each gear (and the final drive) as an
// integer INDEX into a shared teeth-count table, not as a ratio.  NR2003's
// FUN_00425690 turns an index into a ratio with
//   ratio = numerator_teeth / denominator_teeth
// reading the pair from the .rdata table at DAT_006ed880 (denom) /
// DAT_006ed884 (numer), stride 8 bytes.  This module reproduces that table
// byte-for-byte (indices 0..148) so the clean-room garage displays the exact
// same ratios the original does.
//
// Layout of the table (verified against NR2003.exe and 18 shipped setups):
//   idx   0..68  : individual gear ratios  3.5385 .. 0.8846
//   idx  69..99  : {1,1} padding (never selected; gear schema maxes at 68)
//   idx 100..148 : final-drive ratios      2.8571 .. 6.5556
//
// Schema ranges (descriptor blobs, all int/step 1):
//   gear1 0..34(def 12)  gear2 8..59(23)  gear3 15..68(44)
//   gear4 22..68(65)     gear5 22..68(66) gear6 22..68(67)
//   final drive 100..148(def 127)         gear count 0..9(def 4)
constexpr int kGearRatioTableSize = 149;

// Default gear indices for a 4-speed NASCAR gearbox (schema defaults).
// Gears 5/6 are not stored in the 272-byte v13 DGTS body; they retain the
// schema defaults 66/67 in the live setup object.
constexpr int kDefaultGear5Index = 66;
constexpr int kDefaultGear6Index = 67;
constexpr int kDefaultGearCount  = 4;

// Returns numerator_teeth / denominator_teeth for a table index.  Out-of-range
// indices clamp to the table; a zero denominator (never happens for a valid
// index) yields 0 to avoid a divide-by-zero.
double gear_ratio(int index);

}  // namespace opennr
