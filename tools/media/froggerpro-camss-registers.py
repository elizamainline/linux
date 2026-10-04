#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read the powered FroggerPro Lite970 CSID/VFE during an active capture."""

import mmap
import os
from pathlib import Path
import struct
import sys
import time


BASE = 0x0AD6D000
DEVICE = Path("/sys/bus/platform/devices/ad6d000.isp")
CLOCKS = ("cam_cc_ife_lite_clk", "cam_cc_ife_lite_csid_clk")
REGISTERS = {
    "CSID": {
        0x000: "HW_VERSION", 0x018: "RUP_AUP_CMD",
        0x07C: "TOP_IRQ_STATUS", 0x080: "TOP_IRQ_MASK",
        0x08C: "BUF_DONE_IRQ_STATUS", 0x090: "BUF_DONE_IRQ_MASK",
        0x09C: "RX_IRQ_STATUS", 0x0A0: "RX_IRQ_MASK",
        0x200: "RX_CFG0", 0x204: "RX_CFG1",
        0x208: "RX_CAPTURE_CTRL", 0x220: "RX_LONG_PACKET_HEADER",
        0x228: "RX_LONG_PACKET_CRC", 0x240: "RX_PACKET_COUNT",
        0x244: "RX_ECC_STATS", 0x248: "RX_CRC_ERRORS",
    },
    "VFE": {
        0x1000: "HW_VERSION", 0x101C: "TOP_IRQ_STATUS0",
        0x1020: "TOP_IRQ_STATUS1", 0x103C: "CORE_CFG0",
        0x1200: "BUS_VERSION", 0x1208: "BUS_CGC_OVERRIDE",
        0x1218: "BUS_IRQ_MASK", 0x1228: "BUS_IRQ_STATUS",
        0x1264: "BUS_CCIF_VIOLATION", 0x1268: "BUS_OVERFLOW",
        0x1270: "BUS_IMAGE_SIZE_VIOLATION",
    },
}


def require_power():
    compatible = (DEVICE / "of_node/compatible").read_bytes().split(b"\0")
    if b"qcom,eliza-camss" not in compatible:
        raise RuntimeError("This helper requires the Eliza CAMSS register layout")
    if (DEVICE / "power/runtime_status").read_text().strip() != "active":
        raise RuntimeError("CAMSS is suspended; start a capture before reading registers")
    for clock in CLOCKS:
        count = Path("/sys/kernel/debug/clk") / clock / "clk_enable_count"
        if int(count.read_text().strip(), 0) == 0:
            raise RuntimeError(f"{clock} is disabled; start a capture first")


def snapshot(regs, sample):
    def read(offset):
        return struct.unpack_from("<I", regs, offset)[0]

    print(f"Sample {sample}")
    for block, registers in REGISTERS.items():
        for offset, name in registers.items():
            print(f"{block:4s} {offset:04x} {name:26s} {read(offset):08x}")
    for rdi in range(4):
        for offset, name in {
            0xEC: "IRQ_STATUS", 0xF0: "IRQ_MASK", 0x500: "CFG0",
            0x504: "CTRL", 0x510: "CFG1", 0x528: "FRAME_CFG",
            0x538: "CAMIF_DEBUG1", 0x568: "HALT_STATUS",
            0x594: "SOF_TIMESTAMP_LOW", 0x5A4: "EOF_TIMESTAMP_LOW",
        }.items():
            # IRQ registers have a smaller stride than the RDI configuration.
            address = offset + rdi * (0x10 if offset < 0x500 else 0x100)
            print(f"RDI{rdi} {address:04x} {name:26s} {read(address):08x}")
        for offset, name in {
            0x1700: "CFG", 0x1704: "IMAGE_ADDR", 0x1708: "FRAME_INCR",
            0x170C: "IMAGE_CFG0", 0x1714: "STRIDE", 0x1718: "PACKER_CFG",
            0x1770: "ADDR_CFG", 0x1780: "DEBUG_STATUS0",
            0x1784: "DEBUG_STATUS1", 0x1790: "CONSUMED_ADDR0",
            0x1794: "CONSUMED_ADDR1",
        }.items():
            address = offset + rdi * 0x100
            print(f"WM{rdi}  {address:04x} {name:26s} {read(address):08x}")
    sys.stdout.flush()


def main():
    require_power()
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    try:
        with mmap.mmap(fd, 0x3000, flags=mmap.MAP_SHARED,
                       prot=mmap.PROT_READ, offset=BASE) as regs:
            snapshot(regs, 0)
            time.sleep(0.25)
            require_power()
            snapshot(regs, 1)
    finally:
        os.close(fd)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        sys.exit(str(error))
