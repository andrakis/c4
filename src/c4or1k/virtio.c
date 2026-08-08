#include "virtio.h"
#include "mem.h"
#include "cpu.h"
#include "virtio9p.h"

enum {
    REG_MAGIC = 0x0, REG_VERSION = 0x4, REG_DEVICE = 0x8, REG_VENDOR = 0xC,
    REG_HOSTFEATURES = 0x10, REG_HOSTFEATURESSEL = 0x14,
    REG_GUESTFEATURES = 0x20, REG_GUESTFEATURESSEL = 0x24,
    REG_GUEST_PAGE_SIZE = 0x28,
    REG_QUEUESEL = 0x30, REG_QUEUENUMMAX = 0x34, REG_QUEUENUM = 0x38, REG_QUEUEALIGN = 0x3C,
    REG_QUEUEPFN = 0x40, REG_QUEUE_READY = 0x44, REG_QUEUENOTIFY = 0x50,
    REG_INTERRUPTSTATUS = 0x60, REG_INTERRUPTACK = 0x64, REG_STATUS = 0x70,
    REG_QUEUE_DESC_LOW = 0x80, REG_QUEUE_DESC_HIGH = 0x84,
    REG_QUEUE_AVAIL_LOW = 0x90, REG_QUEUE_AVAIL_HIGH = 0x94,
    REG_QUEUE_USED_LOW = 0xA0, REG_QUEUE_USED_HIGH = 0xA4,
    REG_CONFIG_GENERATION = 0xFC
};

enum { VRING_DESC_F_NEXT = 1, VRING_DESC_F_WRITE = 2 };

// The one device this transport hosts: a 9p filesystem (deviceid=9,
// matches Virtio9p's `this.deviceid = 0x9`). hostfeature=0x1 is 9p's
// VIRTIO_9P_MOUNT_TAG bit (dev/virtio/9p.js's `this.hostfeature = 0x1`).
enum { DEVICE_ID = 9, HOST_FEATURE = 0x1 };
// "/dev/root", 2-byte LE length prefix then the string -- matches
// Virtio9p's `this.configspace` exactly. Read by the guest's 9p mount
// code via byte-at-a-time config-space reads (virtio_cread_bytes),
// which is also why this doesn't need the 32-bit configspace-read
// path (below) to be exercised at all in practice.
enum { CONFIGSPACE_LEN = 11 };
int configspace[CONFIGSPACE_LEN];

enum { NQUEUES = 16 };
int queuenum[NQUEUES], queueready[NQUEUES], queuepfn[NQUEUES];
int descaddr[NQUEUES], usedaddr[NQUEUES], availaddr[NQUEUES], lastavailidx[NQUEUES];

int v_status, v_intstatus, v_pagesize, v_align, v_hostfeaturesel, v_queuesel;

int swap32(int v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

int vio_rd_le16(int addr) { return ram_lb(addr) | (ram_lb(addr + 1) << 8); }
void vio_wr_le16(int addr, int val) { ram_sb(addr, val & 0xFF); ram_sb(addr + 1, (val >> 8) & 0xFF); }
int vio_rd_le32(int addr) {
    return ram_lb(addr) | (ram_lb(addr + 1) << 8) | (ram_lb(addr + 2) << 16) | (ram_lb(addr + 3) << 24);
}
void vio_wr_le32(int addr, int val) {
    ram_sb(addr, val & 0xFF); ram_sb(addr + 1, (val >> 8) & 0xFF);
    ram_sb(addr + 2, (val >> 16) & 0xFF); ram_sb(addr + 3, (val >> 24) & 0xFF);
}

void virtio_reset() {
    int i;
    v_status = 0; v_intstatus = 0;
    v_pagesize = 0x2000; v_align = 0x2000;
    v_hostfeaturesel = 0; v_queuesel = 0;
    for (i = 0; i < NQUEUES; ++i) {
        queueready[i] = 0; queuenum[i] = 0x10; queuepfn[i] = 0;
        descaddr[i] = 0; usedaddr[i] = 0; availaddr[i] = 0; lastavailidx[i] = 0;
    }
    configspace[0] = 0x9; configspace[1] = 0x0;
    configspace[2] = '/'; configspace[3] = 'd'; configspace[4] = 'e'; configspace[5] = 'v';
    configspace[6] = '/'; configspace[7] = 'r'; configspace[8] = 'o'; configspace[9] = 'o'; configspace[10] = 't';
}

// Descriptor table entries are 16 bytes: le64 addr (we only ever use
// the low 32 bits -- guest addresses fit easily), le32 len, le16
// flags, le16 next. Matches marshall.Unmarshall(["d","w","h","h"], ...)
// in virtio.js's GetDescriptor.
void virtio_get_descriptor(int queueidx, int index, int *addr_out, int *len_out, int *flags_out, int *next_out) {
    int base;
    base = descaddr[queueidx] + index * 16;
    *addr_out = vio_rd_le32(base);
    *len_out = vio_rd_le32(base + 8);
    *flags_out = vio_rd_le16(base + 12);
    *next_out = vio_rd_le16(base + 14);
}

void virtio_consume_descriptor(int queueidx, int descindex, int desclen) {
    int usedidxaddr, index, entryaddr;
    usedidxaddr = usedaddr[queueidx] + 2;
    index = vio_rd_le16(usedidxaddr);
    vio_wr_le16(usedidxaddr, index + 1);
    entryaddr = usedaddr[queueidx] + 4 + (index & (queuenum[queueidx] - 1)) * 8;
    vio_wr_le32(entryaddr, descindex);
    vio_wr_le32(entryaddr + 4, desclen);
}

void virtio_send_reply(int queueidx, int descindex, char *replybuffer, int replybuffersize) {
    int addr, len, flags, next, offset, i;

    virtio_consume_descriptor(queueidx, descindex, replybuffersize);

    if (replybuffersize == 0) {
        v_intstatus = 1;
        cpu_raise_interrupt(VIRTIO_INTNO);
        return;
    }

    virtio_get_descriptor(queueidx, descindex, &addr, &len, &flags, &next);
    while (!(flags & VRING_DESC_F_WRITE)) {
        if (!(flags & VRING_DESC_F_NEXT)) {
            printf("virtio: descriptor chain has no write-only buffer for reply\n");
            return;
        }
        virtio_get_descriptor(queueidx, next, &addr, &len, &flags, &next);
    }

    offset = 0;
    for (i = 0; i < replybuffersize; ++i) {
        if (offset >= len) {
            virtio_get_descriptor(queueidx, next, &addr, &len, &flags, &next);
            offset = 0;
        }
        ram_sb(addr + offset, replybuffer[i] & 0xFF);
        ++offset;
    }

    v_intstatus = 1;
    cpu_raise_interrupt(VIRTIO_INTNO);
}

int gb_queueidx, gb_addr, gb_len, gb_flags, gb_next, gb_offset;

void virtio_stream_start(int queueidx, int descindex) {
    gb_queueidx = queueidx;
    virtio_get_descriptor(queueidx, descindex, &gb_addr, &gb_len, &gb_flags, &gb_next);
    gb_offset = 0;
}

int virtio_get_byte() {
    int b;
    if (gb_offset >= gb_len) {
        if (!(gb_flags & VRING_DESC_F_NEXT)) {
            printf("virtio: descriptor is not continuing (GetByte ran off the end)\n");
            return 0;
        }
        virtio_get_descriptor(gb_queueidx, gb_next, &gb_addr, &gb_len, &gb_flags, &gb_next);
        gb_offset = 0;
    }
    b = ram_lb(gb_addr + gb_offset);
    gb_offset = gb_offset + 1;
    return b;
}

int virtio_read8(int addr) {
    if (addr >= 0x100) return configspace[addr - 0x100] & 0xFF;
    printf("virtio_read8: unsupported control register 0x%x\n", addr);
    return 0;
}

void virtio_write8(int addr, int val) {
    printf("virtio_write8: unsupported register 0x%x = 0x%x (9p never has its config space written)\n", addr, val);
}

// Control registers are fixed-format little-endian values (the
// virtio-mmio spec's bus convention); the OR1000 guest kernel's own
// readl()/writel() macros do their own le32_to_cpu/cpu_to_le32 swap on
// top of this project's big-endian CPU/RAM model. Each side's swap
// cancels the other's, so *logical* register values (0x74726976 for
// the magic register, etc) only come out correctly if this layer
// deliberately swaps once more -- exactly matching virtio.js's
// ReadReg32 (`Swap32(val)` before returning, for a big-endian target)
// and WriteReg32 (`val = Swap32(val)` before interpreting it). This
// is why mem.c's MMIO branch of ram_lw/ram_sw hands `addr`/`val`
// straight through with no byte reassembly of its own -- all the
// endian handling for control registers lives here, matching where
// jor1k puts it (ram.js's Read32Big/Write32Big call the device
// directly, no RAM-level byte composition for an MMIO address).
int virtio_read32(int addr) {
    int val;

    if (addr >= 0x100) { // config-space 32-bit read; see virtio_read8's comment -- unexercised in practice
        return (configspace[addr - 0x100] << 24) | (configspace[addr - 0x100 + 1] << 16) |
               (configspace[addr - 0x100 + 2] << 8) | configspace[addr - 0x100 + 3];
    }

    switch (addr) {
    case REG_MAGIC: val = 0x74726976; break; // "virt"
    case REG_VERSION: val = 2; break; // modern virtio, matches Linux > 4.0 requirement
    case REG_DEVICE: val = DEVICE_ID; break;
    case REG_VENDOR: val = 0xFFFFFFFF; break;
    case REG_HOSTFEATURES: val = v_hostfeaturesel == 0 ? HOST_FEATURE : (v_hostfeaturesel == 1 ? 1 : 0); break;
    case REG_QUEUENUMMAX: val = queuenum[v_queuesel]; break;
    case REG_QUEUEPFN: val = queuepfn[v_queuesel]; break;
    case REG_QUEUE_READY: val = queueready[v_queuesel]; break;
    case REG_INTERRUPTSTATUS: val = v_intstatus; break;
    case REG_STATUS: val = v_status; break;
    case REG_CONFIG_GENERATION: val = 0; break;
    default: printf("virtio_read32: unsupported register 0x%x\n", addr); val = 0;
    }

    return swap32(val);
}

void virtio_process_queue(int queueidx) {
    int availidx, currentavailidx, currentdescindex;

    availidx = vio_rd_le16(availaddr[queueidx] + 2);
    while (lastavailidx[queueidx] != availidx) {
        currentavailidx = lastavailidx[queueidx] & (queuenum[queueidx] - 1);
        currentdescindex = vio_rd_le16(availaddr[queueidx] + 4 + currentavailidx * 2);

        virtio_stream_start(queueidx, currentdescindex);
        virtio9p_receive_request(queueidx, currentdescindex);

        lastavailidx[queueidx] = (lastavailidx[queueidx] + 1) & 0xFFFF;
    }
}

void virtio_write32(int addr, int val) {
    val = swap32(val); // recover the logical value -- see virtio_read32's comment

    switch (addr) {
    case REG_GUEST_PAGE_SIZE: v_pagesize = val; break;
    case REG_HOSTFEATURESSEL: v_hostfeaturesel = val; break;
    case REG_GUESTFEATURESSEL: break;
    case REG_GUESTFEATURES: break;
    case REG_QUEUESEL: v_queuesel = val; break;
    case REG_QUEUENUM: queuenum[v_queuesel] = val; break;
    case REG_QUEUEALIGN: v_align = val; v_pagesize = val; break;
    case REG_QUEUEPFN: queuepfn[v_queuesel] = val; break;
    case REG_QUEUENOTIFY: virtio_process_queue(val); break;
    case REG_QUEUE_READY: queueready[v_queuesel] = val; break;
    case REG_INTERRUPTACK: v_intstatus = v_intstatus & ~val; cpu_clear_interrupt(VIRTIO_INTNO); break;
    case REG_STATUS:
        v_status = val;
        if (val == 0) { v_intstatus = 0; cpu_clear_interrupt(VIRTIO_INTNO); virtio_reset(); }
        break;
    case REG_QUEUE_DESC_LOW: descaddr[v_queuesel] = val; break;
    case REG_QUEUE_DESC_HIGH: break;
    case REG_QUEUE_AVAIL_LOW: availaddr[v_queuesel] = val; lastavailidx[v_queuesel] = vio_rd_le16(availaddr[v_queuesel] + 2); break;
    case REG_QUEUE_AVAIL_HIGH: break;
    case REG_QUEUE_USED_LOW: usedaddr[v_queuesel] = val; break;
    case REG_QUEUE_USED_HIGH: break;
    default: printf("virtio_write32: unsupported register 0x%x = 0x%x\n", addr, val);
    }
}
