import sys
import os
import re
import pickle
import numpy as np
import json
import argparse
import importlib.resources
from apycula import codegen
from apycula import chipdb
from apycula import bslib
from apycula.wirenames import wirenames, wirenumbers

_verilog_name = re.compile(r"^[A-Za-z_0-9][A-Za-z_0-9$]*$")
def sanitize_name(name):
    retname = name
    if name[-3:] == '_LC':
        retname = name[:-3]
    elif name[-6:] == '_DFFLC':
        retname = name[:-6]
    elif name[-4:] == '$iob':
        retname = name[:-4]
    if _verilog_name.fullmatch(retname):
        return retname
    return f"\{retname} "

def get_bels(data):
    belre = re.compile(r"R(\d+)C(\d+)_(?:SLICE|IOB)(\w)")
    for cellname, cell in data['modules']['top']['cells'].items():
        bel = cell['attributes']['NEXTPNR_BEL']
        row, col, num = belre.match(bel).groups()
        yield (cell['type'], int(row), int(col), num,
                cell['parameters'], cell['attributes'], sanitize_name(cellname))

def get_pips(data):
    pipre = re.compile(r"R(\d+)C(\d+)_([^_]+)_([^_]+)")
    for net in data['modules']['top']['netnames'].values():
        routing = net['attributes']['ROUTING']
        pips = routing.split(';')[1::3]
        for pip in pips:
            res = pipre.fullmatch(pip) # ignore alias
            if res:
                row, col, src, dest = res.groups()
                yield int(row), int(col), src, dest
            elif pip:
                print("Invalid pip:", pip)

def infovaluemap(infovalue, start=2):
    return {tuple(iv[:start]):iv[start:] for iv in infovalue}

iostd_alias = {
        "HSTL18_II"  : "HSTL18_I",
        "SSTL18_I"   : "HSTL18_I",
        "SSTL18_II"  : "HSTL18_I",
        "HSTL15_I"   : "SSTL15",
        "SSTL25_II"  : "SSTL25_I",
        "SSTL33_II"  : "SSTL33_I",
        "LVTTL33"    : "LVCMOS33",
        }
_banks = {}
_sides = "AB"
def place(db, tilemap, bels, cst):
    for typ, row, col, num, parms, attrs, cellname in bels:
        tiledata = db.grid[row-1][col-1]
        tile = tilemap[(row-1, col-1)]
        if typ == "SLICE":
            lutmap = tiledata.bels[f'LUT{num}'].flags
            init = str(parms['INIT'])
            init = init*(16//len(init))
            for bitnum, lutbit in enumerate(init[::-1]):
                if lutbit == '0':
                    fuses = lutmap[bitnum]
                    for brow, bcol in fuses:
                        tile[brow][bcol] = 1

            if int(num) < 6:
                mode = str(parms['FF_TYPE']).strip('E')
                dffbits = tiledata.bels[f'DFF{num}'].modes[mode]
                for brow, bcol in dffbits:
                    tile[brow][bcol] = 1
            # XXX skip power
            if not cellname.startswith('\$PACKER'):
                cst.cells[cellname] = f"R{row}C{col}[{int(num) // 2}][{_sides[int(num) % 2]}]"

        elif typ == "IOB":
            edge = 'T'
            idx = col;
            if row == db.rows:
                edge = 'B'
            elif col == 1:
                edge = 'L'
                idx = row
            elif col == db.cols:
                edge = 'R'
                idx = row
            cst.ports[cellname] = f"IO{edge}{idx}{num}"
            iob = tiledata.bels[f'IOB{num}']
            if int(parms["ENABLE_USED"], 2) and int(parms["OUTPUT_USED"], 2):
                # TBUF = IOBUF - O
                mode = "IOBUF"
            elif int(parms["INPUT_USED"], 2):
                mode = "IBUF"
            elif int(parms["OUTPUT_USED"], 2):
                mode = "OBUF"
            else:
                raise ValueError("IOB has no in or output")

            bank = chipdb.loc2bank(db, row - 1, col - 1)
            iostd = _banks.setdefault(bank, None)

            # find io standard
            for flag in attrs.keys():
                flag_name_val = flag.split("=")
                if len(flag_name_val) < 2:
                    continue
                if flag[0] != chipdb.mode_attr_sep:
                    continue
                if flag_name_val[0] == chipdb.mode_attr_sep + "IO_TYPE":
                    if iostd and iostd != flag_name_val[1]:
                        raise Exception("Different I/O modes for the same bank were specified: " +
                                f"{iostd} and {flag_name_val[1]}")
                    iostd = iostd_alias.get(flag_name_val[1], flag_name_val[1])

            # first used pin sets bank's iostd
            # XXX default io standard may be board-dependent!
            if not iostd:
                iostd = "LVCMOS18"
            _banks[bank] = iostd

            cst.attrs.setdefault(cellname, {}).update({"IO_TYPE": iostd})
            # collect flag bits
            bits = iob.iob_flags[iostd][mode].encode_bits
            for flag in attrs.keys():
                flag_name_val = flag.split("=")
                if len(flag_name_val) < 2:
                    continue
                if flag[0] != chipdb.mode_attr_sep:
                    continue
                if flag_name_val[0] == chipdb.mode_attr_sep + "IO_TYPE":
                    continue
                # set flag
                mode_desc = iob.iob_flags[iostd][mode]
                try:
                   flag_desc = mode_desc.flags[flag_name_val[0][1:]]
                   flag_bits = flag_desc.options[flag_name_val[1]]
                except KeyError:
                    raise Exception(
                            f"Incorrect attribute {flag[1:]} (iostd:\"{iostd}\", mode:{mode})")
                bits -= flag_desc.mask
                bits.update(flag_bits)
                cst.attrs[cellname].update({flag_name_val[0][1:] : flag_name_val[1]})
            for r, c in bits:
                tile[r][c] = 1

            #bank enable
            for pos, bnum in db.corners.items():
                if bnum == bank:
                    break
            brow, bcol = pos
            tiledata = db.grid[brow][bcol]
            tile = tilemap[(brow, bcol)]
            if not len(tiledata.bels) == 0:
                bank_bel = tiledata.bels['BANK']

                bits = bank_bel.modes['ENABLE']
                # iostd flag
                bits |= bank_bel.bank_flags[iostd]
                for row, col in bits:
                    tile[row][col] = 1


def route(db, tilemap, pips):
    for row, col, src, dest in pips:
        tiledata = db.grid[row-1][col-1]
        tile = tilemap[(row-1, col-1)]

        try:
            if dest in tiledata.clock_pips:
                bits = tiledata.clock_pips[dest][src]
            else:
                bits = tiledata.pips[dest][src]
        except KeyError:
            print(src, dest, "not found in tile", row, col)
            breakpoint()
            continue
        for row, col in bits:
            tile[row][col] = 1

def header_footer(db, bs, compress):
    """
    Generate fs header and footer
    Currently limited to checksum with
    CRC_check and security_bit_enable set
    """
    bs = np.fliplr(bs)
    bs=np.packbits(bs)
    # configuration data checksum is computed on all
    # data in 16bit format
    bb = np.array(bs)

    res = int(bb[0::2].sum() * pow(2,8) + bb[1::2].sum())
    checksum = res & 0xffff
    db.cmd_hdr[0] = bytearray.fromhex(f"{checksum:04x}")

    if compress:
        # update line 0x10 with compress enable bit
        # rest (keys) is done in bslib.write_bitstream
        hdr10 = int.from_bytes(db.cmd_hdr[4], 'big') | (1 << 13)
        db.cmd_hdr[4] = bytearray.fromhex(f"{hdr10:016x}")

    # same task for line 2 in footer
    db.cmd_ftr[1] = bytearray.fromhex(f"{0x0A << 56 | checksum:016x}")

def main():
    parser = argparse.ArgumentParser(description='Pack Gowin bitstream')
    parser.add_argument('netlist')
    parser.add_argument('-d', '--device', required=True)
    parser.add_argument('-o', '--output', default='pack.fs')
    parser.add_argument('-c', '--compress', default=False, action='store_true')
    parser.add_argument('-s', '--cst', default=None)
    parser.add_argument('--png')

    args = parser.parse_args()

    device = args.device
    # For tool integration it is allowed to pass a full part number
    m = re.match("GW1N([A-Z]*)-(LV|UV|UX)([0-9])C?([A-Z]{2}[0-9]+)(C[0-9]/I[0-9])", device)
    if m:
        luts = m.group(3)
        device = f"GW1N-{luts}"

    with importlib.resources.open_binary("apycula", f"{device}.pickle") as f:
        db = pickle.load(f)
    with open(args.netlist) as f:
        pnr = json.load(f)

    tilemap = chipdb.tile_bitmap(db, db.template, empty=True)
    cst = codegen.Constraints()
    bels = get_bels(pnr)
    place(db, tilemap, bels, cst)
    pips = get_pips(pnr)
    route(db, tilemap, pips)
    res = chipdb.fuse_bitmap(db, tilemap)
    header_footer(db, res, args.compress)
    if args.png:
        bslib.display(args.png, res)
    bslib.write_bitstream(args.output, res, db.cmd_hdr, db.cmd_ftr, args.compress)
    if args.cst:
        with open(args.cst, "w") as f:
                cst.write(f)

if __name__ == '__main__':
    main()
