// bels/dsp.hpp - DSP type-specific attribute functions
#pragma once

#include "../chipdb_types.hpp"
#include "../place.hpp"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace apycula {
namespace dsp {

// Constants matching Python _01LH and _ABLH
static const std::vector<std::pair<int, char>> _01LH = {{0, 'L'}, {1, 'H'}};
static const std::vector<std::pair<char, char>> _ABLH = {{'A', 'L'}, {'A', 'H'}, {'B', 'L'}, {'B', 'H'}};

// Helper: ensure register params have default value "0"
void set_dsp_regs_0(std::map<std::string, std::string>& parms,
                     const std::set<std::string>& names);

// Helper: uppercase all string values in attrs
void attrs_upper(std::map<std::string, std::string>& attrs);

// Type-specific DSP attribute setters
void set_multalu18x18_attrs(const Device& db, const std::string& typ,
    std::map<std::string, std::string>& params, const std::string& num,
    std::map<std::string, std::string>& attrs,
    std::map<std::string, std::string>& dsp_attrs, int mac);

void set_multaddalu18x18_attrs(const Device& db, const std::string& typ,
    std::map<std::string, std::string>& params, const std::string& num,
    std::map<std::string, std::string>& attrs,
    std::map<std::string, std::string>& dsp_attrs, int mac);

void set_multalu36x18_attrs(const Device& db, const std::string& typ,
    std::map<std::string, std::string>& params, const std::string& num,
    std::map<std::string, std::string>& attrs,
    std::map<std::string, std::string>& dsp_attrs, int mac);

void set_alu54d_attrs(const Device& db, const std::string& typ,
    std::map<std::string, std::string>& params, const std::string& num,
    std::map<std::string, std::string>& attrs,
    std::map<std::string, std::string>& dsp_attrs, int mac);

void set_padd9_attrs(const Device& db, const std::string& typ,
    std::map<std::string, std::string>& params, const std::string& num,
    std::map<std::string, std::string>& attrs,
    std::map<std::string, std::string>& dsp_attrs, int mac,
    int idx, int even_odd, int pair_idx);

void set_mult9x9_attrs(const Device& db, const std::string& typ,
    std::map<std::string, std::string>& params, const std::string& num,
    std::map<std::string, std::string>& attrs,
    std::map<std::string, std::string>& dsp_attrs, int mac,
    int idx, int even_odd, int pair_idx);

// Main DSP dispatcher - returns fin_attrs set
std::set<int64_t> set_dsp_attrs(const Device& db, const std::string& typ,
    std::map<std::string, std::string>& params, const std::string& num,
    std::map<std::string, std::string>& attrs);

// Special case for MULT36X36 - returns two fin_attrs sets (one per macro)
std::vector<std::set<int64_t>> set_dsp_mult36x36_attrs(const Device& db,
    const std::string& typ,
    std::map<std::string, std::string>& params,
    std::map<std::string, std::string>& attrs);

} // namespace dsp
} // namespace apycula
