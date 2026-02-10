// route.cpp - Routing implementation
#include "route.hpp"
#include "place.hpp"
#include "fuses.hpp"
#include "attrids.hpp"
#include <regex>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace apycula {

// Map LUT input letter to pass-through INIT value
static const std::map<char, std::string> passthrough_init = {
    {'A', "1010101010101010"},
    {'B', "1100110011001100"},
    {'C', "1111000011110000"},
    {'D', "1111111100000000"},
};

std::vector<Pip> get_pips(const Netlist& netlist, std::vector<BelInfo>& pip_bels) {
    std::vector<Pip> pips;
    std::regex pip_re(R"(X(\d+)Y(\d+)/([\w_]+)/([\w_]+))");

    for (const auto& [name, net] : netlist.nets) {
        auto routing_it = net.attributes.find("ROUTING");
        if (routing_it == net.attributes.end()) continue;

        // Get routing string
        std::string routing;
        if (auto* s = std::get_if<std::string>(&routing_it->second)) {
            routing = *s;
        } else {
            continue;
        }

        // Parse PIPs from routing string
        // Format: wire;pip;wire;pip;wire;pip;...
        // PIPs are at indices 1, 4, 7, ... (every 3rd element starting from index 1)
        size_t pos = 0;
        int count = 0;
        while (pos < routing.size()) {
            size_t next = routing.find(';', pos);
            if (next == std::string::npos) next = routing.size();
            std::string segment = routing.substr(pos, next - pos);
            pos = next + 1;

            // Every 3rd segment starting from index 1 is a pip
            if (count % 3 == 1 && !segment.empty()) {
                std::smatch match;
                if (std::regex_match(segment, match, pip_re)) {
                    // Regex groups: X(col_val) Y(row_val) / wire1 / wire2
                    // Following Python: col = X_val + 1, row = Y_val + 1
                    // dest = wire1 (group 3), src = wire2 (group 4)
                    int64_t x_val = std::stoll(match[1].str());
                    int64_t y_val = std::stoll(match[2].str());
                    std::string dest = match[3].str();
                    std::string src = match[4].str();

                    // XD - input of the DFF: needs special handling
                    // Note: in get_pips, match[3] is called "src" in Python
                    // (which becomes "dest" in route_nets consumer). We use
                    // C++ naming that matches route_nets: dest=match[3], src=match[4].
                    // Python checks its "src" (=our dest) for XD prefix.
                    if (dest.size() >= 2 && dest[0] == 'X' && dest[1] == 'D') {
                        if (src.size() >= 1 && src[0] == 'F') {
                            // XD -> F: skip entirely
                            count++;
                            continue;
                        }
                        // Pass-through LUT: src (Python's "dest") is like "A5", "B3"
                        // src[0] is the LUT input letter, src[1] is the slice number
                        char lut_input = src[0];
                        std::string slice_num(1, src[1]);
                        auto init_it = passthrough_init.find(lut_input);
                        if (init_it != passthrough_init.end()) {
                            BelInfo bel;
                            bel.type = "LUT4";
                            bel.col = x_val + 1;
                            bel.row = y_val + 1;
                            bel.num = slice_num;
                            bel.parameters["INIT"] = init_it->second;
                            bel.name = "$PACKER_PASS_LUT_" + std::to_string(pip_bels.size());
                            bel.cell = nullptr;
                            pip_bels.push_back(std::move(bel));
                        }
                        count++;
                        continue;
                    }

                    Pip pip;
                    pip.col = x_val + 1;
                    pip.row = y_val + 1;
                    pip.dest = dest;
                    pip.src = src;
                    pips.push_back(pip);
                } else if (segment.find("DUMMY") == std::string::npos) {
                    std::cerr << "Invalid pip: " << segment << std::endl;
                }
            }
            count++;
        }
    }
    return pips;
}

void isolate_segments(
    const Device& db,
    const Netlist& netlist,
    Tilemap& tilemap) {

    std::regex wire_re(R"(X(\d+)Y(\d+)/([\w]+))");

    for (const auto& [name, net] : netlist.nets) {
        auto seg_it = net.attributes.find("SEG_WIRES_TO_ISOLATE");
        if (seg_it == net.attributes.end()) continue;

        std::string wires_str;
        if (auto* s = std::get_if<std::string>(&seg_it->second)) {
            wires_str = *s;
        } else {
            continue;
        }

        // Parse semicolon-separated wire list: "X{col}Y{row}/{wire};..."
        size_t pos = 0;
        while (pos < wires_str.size()) {
            size_t next = wires_str.find(';', pos);
            if (next == std::string::npos) next = wires_str.size();
            std::string wire_ex = wires_str.substr(pos, next - pos);
            pos = next + 1;

            if (wire_ex.empty()) continue;

            std::smatch res;
            if (!std::regex_match(wire_ex, res, wire_re)) {
                throw std::runtime_error("Invalid isolated wire:" + wire_ex);
            }

            // X -> col, Y -> row (0-indexed coordinates)
            int64_t col = std::stoll(res[1].str());
            int64_t row = std::stoll(res[2].str());
            std::string wire = res[3].str();

            const auto& tiledata = db.get_tile(row, col);
            auto& tile = tilemap[{row, col}];

            auto alone_it = tiledata.alonenode_6.find(wire);
            if (alone_it == tiledata.alonenode_6.end()) {
                throw std::runtime_error(
                    "Wire " + wire + " is not in alonenode fuse table");
            }
            if (alone_it->second.size() != 1) {
                throw std::runtime_error(
                    "Incorrect alonenode fuse table for " + wire);
            }

            // Get fuse bits from the single entry's second element (the fuse set)
            const auto& bits = alone_it->second[0].second;
            for (const auto& [brow, bcol] : bits) {
                if (brow >= 0 && brow < static_cast<int64_t>(tile.size()) &&
                    bcol >= 0 && bcol < static_cast<int64_t>(tile[brow].size())) {
                    tile[brow][bcol] = 1;
                }
            }
        }
    }
}

// Forward declarations for GW5A clock routing (defined below)
static bool is_clock_pip(const std::string& device, const std::string& src,
                         const std::string& dest);

std::vector<BelInfo> route_nets(
    const Device& db,
    const Netlist& netlist,
    Tilemap& tilemap,
    const std::string& device) {

    std::vector<BelInfo> pip_bels;
    auto pips = get_pips(netlist, pip_bels);

    bool is_gw5a = (device == "GW5A-25A" || device == "GW5AST-138C");

    for (const auto& pip : pips) {
        // PIPs use 1-indexed coordinates; convert to 0-indexed for tile access
        int64_t row = pip.row - 1;
        int64_t col = pip.col - 1;

        if (row < 0 || row >= static_cast<int64_t>(db.rows()) ||
            col < 0 || col >= static_cast<int64_t>(db.cols())) {
            continue;
        }

        // GW5A clock PIPs need special global handling
        if (is_gw5a && is_clock_pip(device, pip.src, pip.dest)) {
            set_clock_fuses(db, tilemap, pip.row, pip.col, pip.src, pip.dest, device);
            continue;
        }

        const auto& tiledata = db.get_tile(row, col);
        auto& tile = tilemap[{row, col}];

        // Look up PIP fuses
        std::set<Coord> bits;
        bool found = false;

        // Check clock_pips first
        {
            auto clock_it = tiledata.clock_pips.find(pip.dest);
            if (clock_it != tiledata.clock_pips.end()) {
                auto src_it = clock_it->second.find(pip.src);
                if (src_it != clock_it->second.end()) {
                    bits = src_it->second;
                    found = true;
                }
            }
        }

        // Check HCLK pips (uses 0-indexed coordinates)
        if (!found) {
            Coord hclk_coord = {row, col};
            auto hclk_it = db.hclk_pips.find(hclk_coord);
            if (hclk_it != db.hclk_pips.end()) {
                auto dest_it = hclk_it->second.find(pip.dest);
                if (dest_it != hclk_it->second.end()) {
                    auto src_it = dest_it->second.find(pip.src);
                    if (src_it != dest_it->second.end()) {
                        bits = src_it->second;
                        found = true;
                        // HCLK interbank fuses
                        if (pip.dest == "HCLK_BANK_OUT0" || pip.dest == "HCLK_BANK_OUT1") {
                            char mux_idx = pip.dest.back(); // '0' or '1'
                            std::string attr_name = std::string("BRGMUX") + mux_idx + "_BRGOUT";
                            auto attr_it = attrids::hclk_attrids.find(attr_name);
                            auto val_it = attrids::hclk_attrvals.find("ENABLE");
                            if (attr_it != attrids::hclk_attrids.end() &&
                                val_it != attrids::hclk_attrvals.end()) {
                                std::set<int64_t> fin_attrs;
                                add_attr_val(db, "HCLK", fin_attrs, attr_it->second, val_it->second);
                                int64_t ttyp = db.get_ttyp(row, col);
                                auto hclk_fuses = get_shortval_fuses(db, ttyp, fin_attrs, "HCLK");
                                bits.insert(hclk_fuses.begin(), hclk_fuses.end());
                            }
                        }
                    }
                }
            }
        }

        // Check regular pips
        if (!found) {
            auto pip_it = tiledata.pips.find(pip.dest);
            if (pip_it != tiledata.pips.end()) {
                auto src_it = pip_it->second.find(pip.src);
                if (src_it != pip_it->second.end()) {
                    bits = src_it->second;
                    found = true;
                }
            }

            // Check alonenode for isolation fuses (only for regular pips)
            if (found) {
                auto alone_it = tiledata.alonenode.find(pip.dest);
                if (alone_it != tiledata.alonenode.end()) {
                    for (const auto& [srcs, fuses] : alone_it->second) {
                        if (srcs.find(pip.src) == srcs.end()) {
                            // Source not in allowed set, add isolation fuses
                            bits.insert(fuses.begin(), fuses.end());
                        }
                    }
                }
            }
        }

        if (!found) {
            std::cerr << pip.src << " " << pip.dest
                      << " not found in tile " << pip.row << " " << pip.col
                      << std::endl;
            continue;
        }

        // Set the fuse bits
        for (const auto& [brow, bcol] : bits) {
            if (brow >= 0 && brow < static_cast<int64_t>(tile.size()) &&
                bcol >= 0 && bcol < static_cast<int64_t>(tile[brow].size())) {
                tile[brow][bcol] = 1;
            }
        }
    }

    // Isolate segments after PIP routing
    isolate_segments(db, netlist, tilemap);

    return pip_bels;
}

// ============================================================================
// GW5A Clock Routing
// ============================================================================

// Build clknumbers map (name -> index) for GW5A-25A
static const std::map<std::string, int>& get_clknumbers_5a25a() {
    static std::map<std::string, int> m;
    if (!m.empty()) return m;
    // SPINE0-31
    for (int n = 0; n < 32; n++) m["SPINE" + std::to_string(n)] = n;
    // LWT0-7, LWB0-7
    for (int n = 0; n < 8; n++) m["LWT" + std::to_string(n)] = 32 + n;
    for (int n = 0; n < 8; n++) m["LWB" + std::to_string(n)] = 40 + n;
    // Primary clock wires
    static const char* pnames[] = {
        "P16A","P16B","P16C","P16D","P17A","P17B","P17C","P17D",
        "P26A","P26B","P26C","P26D","P27A","P27B","P27C","P27D",
        "P36A","P36B","P36C","P36D","P37A","P37B","P37C","P37D",
        "P46A","P46B","P46C","P46D","P47A","P47B","P47C","P47D"
    };
    for (int i = 0; i < 32; i++) m[pnames[i]] = 48 + i;
    m["VSS"] = 80;
    // PLL outputs
    static const char* pll_prefixes[] = {"PLL4","PLL3","PLL2","PLL8","PLL6","PLL5"};
    for (int p = 0; p < 6; p++) {
        for (int i = 0; i < 8; i++)
            m[std::string(pll_prefixes[p]) + "CLKOUT" + std::to_string(i)] = 81 + p*8 + i;
    }
    // Boundary distribution clocks
    static const char* bd_names[] = {
        "TRBDCLK0","TRBDCLK1","TRBDCLK2","TRBDCLK3",
        "TLBDCLK0","TLBDCLK1","TLBDCLK2","TLBDCLK3",
        "BRBDCLK0","BRBDCLK1","BRBDCLK2","BRBDCLK3",
        "BLBDCLK0","BLBDCLK1","BLBDCLK2","BLBDCLK3",
        "TRMDCLK0","TRMDCLK1","TLMDCLK0","TLMDCLK1",
        "BRMDCLK0","BRMDCLK1","BLMDCLK0","BLMDCLK1"
    };
    for (int i = 0; i < 24; i++) m[bd_names[i]] = 129 + i;
    // UNK ranges
    for (int n = 153; n < 170; n++) m["UNK" + std::to_string(n)] = n;
    // HCLK distribution
    static const char* hd_names[] = {
        "TBDHCLK0","TBDHCLK1","TBDHCLK2","TBDHCLK3",
        "RBDHCLK0","RBDHCLK1","RBDHCLK2","RBDHCLK3",
        "BBDHCLK0","BBDHCLK1","BBDHCLK2","BBDHCLK3",
        "LBDHCLK0","LBDHCLK1","LBDHCLK2","LBDHCLK3"
    };
    for (int i = 0; i < 16; i++) m[hd_names[i]] = 169 + i;
    for (int n = 185; n < 277; n++) m["UNK" + std::to_string(n)] = n;
    m["VCC"] = 277;
    for (int n = 278; n < 281; n++) m["UNK" + std::to_string(n)] = n;
    m["GT00"] = 291; m["GT10"] = 292;
    for (int n = 309; n < 335; n++) m["UNK" + std::to_string(n)] = n;
    // MPLL outputs
    static const char* mpll_prefixes[] = {"MPLL4","MPLL3","MPLL2","MPLL8","MPLL6","MPLL5"};
    for (int p = 0; p < 6; p++) {
        int base = 501 + p * 11;
        for (int i = 0; i < 7; i++)
            m[std::string(mpll_prefixes[p]) + "CLKOUT" + std::to_string(i)] = base + i;
        m[std::string(mpll_prefixes[p]) + "CLKFBOUT"] = base + 7;
        m[std::string(mpll_prefixes[p]) + "CLKIN2"] = base + 8;
        m[std::string(mpll_prefixes[p]) + "CLKIN6"] = base + 9;
        m[std::string(mpll_prefixes[p]) + "CLKIN7"] = base + 10;
    }
    for (int n = 567; n < 570; n++) m["UNK" + std::to_string(n)] = n;
    // HCLK mux (1000 survives, 1001+ overwritten by LWSPINE)
    m["HCLKMUX0"] = 1000;
    // LWSPINE
    for (int n = 0; n < 8; n++) m["LWSPINETL" + std::to_string(n)] = 1001 + n;
    for (int n = 0; n < 8; n++) m["LWSPINETR" + std::to_string(n)] = 1009 + n;
    for (int n = 0; n < 8; n++) m["LWSPINEBL" + std::to_string(n)] = 1017 + n;
    for (int n = 0; n < 8; n++) m["LWSPINEBR" + std::to_string(n)] = 1025 + n;
    for (int n = 0; n < 8; n++) m["LWSPINEB1L" + std::to_string(n)] = 1033 + n;
    for (int n = 0; n < 8; n++) m["LWSPINEB1R" + std::to_string(n)] = 1041 + n;
    for (int n = 1049; n < 1225; n++) m["UNK" + std::to_string(n)] = n;
    // GW5AST-138C UNK ranges
    for (int base : {1273,1353,1433,1513,1593,1673,1753,1833,1913,1993})
        for (int n = base; n < base + 16; n++) m["UNK" + std::to_string(n)] = n;
    return m;
}

// Build clknumbers map for GW5AST-138C
static const std::map<std::string, int>& get_clknumbers_5ast138c() {
    static std::map<std::string, int> m;
    if (!m.empty()) return m;
    // SPINE0-31
    for (int n = 0; n < 32; n++) m["SPINE" + std::to_string(n)] = n;
    for (int n = 0; n < 8; n++) m["LWT" + std::to_string(n)] = 32 + n;
    for (int n = 0; n < 8; n++) m["LWB" + std::to_string(n)] = 40 + n;
    // Primary clock wires (same as 5a25a)
    static const char* pnames[] = {
        "P16A","P16B","P16C","P16D","P17A","P17B","P17C","P17D",
        "P26A","P26B","P26C","P26D","P27A","P27B","P27C","P27D",
        "P36A","P36B","P36C","P36D","P37A","P37B","P37C","P37D",
        "P46A","P46B","P46C","P46D","P47A","P47B","P47C","P47D"
    };
    for (int i = 0; i < 32; i++) m[pnames[i]] = 48 + i;
    m["VSS"] = 80;
    // PLL outputs (138C uses TLPLL/BLPLL/TRPLL/BRPLL naming)
    static const struct { const char* name; int idx; } pll138c[] = {
        {"TLPLL0CLK0",81},{"TLPLL0CLK1",82},{"TLPLL0CLK2",83},{"TLPLL0CLK3",84},
        {"TLPLL1CLK0",85},{"TLPLL1CLK1",86},{"TLPLL1CLK2",87},{"TLPLL1CLK3",88},
        {"BLPLL0CLK0",89},{"BLPLL0CLK1",90},{"BLPLL0CLK2",91},{"BLPLL0CLK3",92},
        {"TRPLL0CLK0",93},{"TRPLL0CLK1",94},{"TRPLL0CLK2",95},{"TRPLL0CLK3",96},
        {"TRPLL1CLK0",97},{"TRPLL1CLK1",98},{"TRPLL1CLK2",99},{"TRPLL1CLK3",100},
        {"BRPLL0CLK0",101},{"BRPLL0CLK1",102},{"BRPLL0CLK2",103},{"BRPLL0CLK3",104},
    };
    for (const auto& e : pll138c) m[e.name] = e.idx;
    for (int n = 105; n < 131; n++) m["UNK" + std::to_string(n)] = n;
    // External clock pins
    static const struct { const char* name; int idx; } pclk138c[] = {
        {"PCLKT0",131},{"PCLKT1",132},{"PCLKB0",133},{"PCLKB1",134},
        {"PCLKL0",135},{"PCLKL1",136},{"PCLKR0",137},{"PCLKR1",138},
    };
    for (const auto& e : pclk138c) m[e.name] = e.idx;
    // Boundary distribution clocks (different ordering from 5a25a)
    static const struct { const char* name; int idx; } bd138c[] = {
        {"TRBDCLK0",139},{"TRBDCLK1",140},{"TRBDCLK2",141},{"TRBDCLK3",142},
        {"TLBDCLK1",143},{"TLBDCLK2",144},{"TLBDCLK3",145},{"TLBDCLK0",146},
        {"BRBDCLK2",147},{"BRBDCLK3",148},{"BRBDCLK0",149},{"BRBDCLK1",150},
        {"BLBDCLK3",151},{"BLBDCLK0",152},{"BLBDCLK1",153},{"BLBDCLK2",154},
        {"TRMDCLK0",155},{"TLMDCLK0",156},{"BRMDCLK0",157},{"BLMDCLK0",158},
        {"BLMDCLK1",159},{"BRMDCLK1",160},{"TLMDCLK1",161},{"TRMDCLK1",162},
    };
    for (const auto& e : bd138c) m[e.name] = e.idx;
    for (int n = 163; n < 237; n++) m["UNK" + std::to_string(n)] = n;
    // Clock bridge outputs (unique to 138C)
    for (int i = 0; i < 8; i++) m["CBRIDGEOUT_TOP" + std::to_string(i)] = 237 + i;
    for (int i = 0; i < 8; i++) m["CBRIDGEOUT_BOTTOM" + std::to_string(i)] = 245 + i;
    for (int n = 253; n < 309; n++) m["UNK" + std::to_string(n)] = n;
    m["VCC"] = 277;
    m["GT00"] = 291; m["GT10"] = 292;
    for (int n = 309; n < 570; n++) m["UNK" + std::to_string(n)] = n;
    // HCLK mux
    m["HCLKMUX0"] = 1000;
    for (int n = 0; n < 8; n++) m["LWSPINETL" + std::to_string(n)] = 1001 + n;
    for (int n = 0; n < 8; n++) m["LWSPINETR" + std::to_string(n)] = 1009 + n;
    for (int n = 0; n < 8; n++) m["LWSPINEBL" + std::to_string(n)] = 1017 + n;
    for (int n = 0; n < 8; n++) m["LWSPINEBR" + std::to_string(n)] = 1025 + n;
    for (int n = 0; n < 8; n++) m["LWSPINEB1L" + std::to_string(n)] = 1033 + n;
    for (int n = 0; n < 8; n++) m["LWSPINEB1R" + std::to_string(n)] = 1041 + n;
    for (int n = 1049; n < 1225; n++) m["UNK" + std::to_string(n)] = n;
    for (int base : {1273,1353,1433,1513,1593,1673,1753,1833,1913,1993})
        for (int n = base; n < base + 16; n++) m["UNK" + std::to_string(n)] = n;
    return m;
}

static const std::map<std::string, int>& get_clknumbers(const std::string& device) {
    if (device == "GW5AST-138C") return get_clknumbers_5ast138c();
    return get_clknumbers_5a25a();
}

// Matches Python is_clock_pip
static bool is_clock_pip(const std::string& device, const std::string& src,
                         const std::string& dest) {
    // Check src[8:] for _BOT/_TOP prefix
    if (src.size() > 8) {
        auto suffix = src.substr(8);
        if (suffix.substr(0, 4) == "_BOT" || suffix.substr(0, 4) == "_TOP")
            return true;
    }

    const auto& clknumbers = get_clknumbers(device);
    auto src_it = clknumbers.find(src);
    if (src_it == clknumbers.end()) return false;
    auto dest_it = clknumbers.find(dest);
    if (dest_it == clknumbers.end()) return false;

    int src_idx = src_it->second;

    if (device == "GW5A-25A") {
        // UNK212 = 212, MPLL4CLKOUT0 = 501, UNK569 = 569
        return src_idx < 212 || (src_idx >= 501 && src_idx <= 569);
    }
    if (device == "GW5AST-138C") {
        // UNK269 = 269, UNK309 = 309
        return src_idx < 269 || src_idx >= 309;
    }
    return false;
}

void set_clock_fuses(
    const Device& db,
    Tilemap& tilemap,
    int64_t row_,
    int64_t col_,
    const std::string& src,
    const std::string& dest,
    const std::string& device) {

    // SPINE->{GT00, GT10} must be set in the cell only
    if (dest == "GT00" || dest == "GT10") {
        int64_t r = row_ - 1, c = col_ - 1;
        if (r < 0 || r >= static_cast<int64_t>(db.rows()) ||
            c < 0 || c >= static_cast<int64_t>(db.cols())) return;
        const auto& td = db.get_tile(r, c);
        auto& tile = tilemap[{r, c}];
        auto clock_it = td.clock_pips.find(dest);
        if (clock_it != td.clock_pips.end()) {
            auto src_it = clock_it->second.find(src);
            if (src_it != clock_it->second.end()) {
                for (const auto& [brow, bcol] : src_it->second)
                    tile[brow][bcol] = 1;
            }
        }
        return;
    }

    // Determine allowed area for GW5AST-138C top/bottom half separation
    char area = 'T';
    int64_t allowed_row_start = 0;
    int64_t allowed_row_end = static_cast<int64_t>(db.rows());
    // Clock bridge state for 138C
    static const std::set<int64_t> clock_bridge_ttypes_set = {80,81,82,83,84,85};
    std::set<int64_t> clock_bridge_cols_set;
    static const std::set<int64_t> clock_bridge_rows_set = {54};
    bool is_138c = (device == "GW5AST-138C");

    if (is_138c) {
        // Compute clock_bridge_cols
        for (int64_t c = 0; c < static_cast<int64_t>(db.cols()); c++) {
            for (int64_t r = 0; r < static_cast<int64_t>(db.rows()); r++) {
                if (clock_bridge_ttypes_set.count(db.get_ttyp(r, c))) {
                    clock_bridge_cols_set.insert(c);
                    break;
                }
            }
        }

        allowed_row_end = 55; // top half
        int64_t pip_row = row_ - 1;
        if (pip_row >= 55) {
            // bottom half
            allowed_row_start = 55;
            allowed_row_end = static_cast<int64_t>(db.rows());
            area = 'B';
        } else if (clock_bridge_ttypes_set.count(db.get_ttyp(pip_row, col_ - 1))) {
            // clock bridge area - col filtering done via clock_bridge_cols_set in loop
            allowed_row_start = 54;
            allowed_row_end = 55;
            area = 'C';
        }
    }

    // Track used spines per area (persistent across calls within one routing pass)
    static std::set<std::pair<char, std::string>> used_spines;
    std::string spine_enable_table;

    if (dest.substr(0, 5) == "SPINE") {
        auto key = std::make_pair(area, dest);
        if (used_spines.find(key) == used_spines.end()) {
            used_spines.insert(key);
            const auto& clknumbers = get_clknumbers(device);
            auto it = clknumbers.find(dest);
            if (it != clknumbers.end()) {
                char buf[32];
                snprintf(buf, sizeof(buf), "5A_PCLK_ENABLE_%02d", it->second);
                spine_enable_table = buf;
            }
        }
    }

    // Iterate all tiles in allowed area
    for (int64_t row = 0; row < static_cast<int64_t>(db.rows()); row++) {
        if (row < allowed_row_start || row >= allowed_row_end) continue;
        for (int64_t col = 0; col < static_cast<int64_t>(db.cols()); col++) {
            if (is_138c && area == 'C') {
                if (!clock_bridge_cols_set.count(col)) continue;
            } else if (is_138c && area == 'T' &&
                       clock_bridge_rows_set.count(row) &&
                       clock_bridge_cols_set.count(col)) {
                continue;
            }

            const auto& rc = db.get_tile(row, col);
            int64_t ttyp = db.get_ttyp(row, col);
            std::set<Coord> bits;

            // Check clock_pips
            auto clock_it = rc.clock_pips.find(dest);
            if (clock_it != rc.clock_pips.end()) {
                auto src_it = clock_it->second.find(src);
                if (src_it != clock_it->second.end()) {
                    bits = src_it->second;
                }
            }

            // Check spine enable table
            if (!spine_enable_table.empty()) {
                auto ttyp_sv = db.shortval.find(ttyp);
                if (ttyp_sv != db.shortval.end()) {
                    auto table_it = ttyp_sv->second.find(spine_enable_table);
                    if (table_it != ttyp_sv->second.end()) {
                        Coord enable_key = {1, 0};
                        auto key_it = table_it->second.find(enable_key);
                        if (key_it != table_it->second.end()) {
                            bits.insert(key_it->second.begin(), key_it->second.end());
                        }
                    }
                }
            }

            if (!bits.empty()) {
                auto& tile = tilemap[{row, col}];
                for (const auto& [brow, bcol] : bits)
                    tile[brow][bcol] = 1;
            }
        }
    }
}

} // namespace apycula
