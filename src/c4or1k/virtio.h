// c4or1k virtio-mmio transport, matching
// jorconsole/jor1k/js/worker/dev/virtio.js's VirtIODev register-level
// behavior exactly (same offsets, same version=2 "modern virtio"
// negotiation, same descriptor-chain walk). MMIO base 0x97000000, IRQ
// line 6 -- confirmed against system.js's device wiring and against
// the kernel command line baked into vmlinux.bin itself
// (`root=host rootfstype=9p rootflags=trans=virtio`, found via
// `strings vmlinux.bin`; there's no other virtio transport to probe).
//
// Only one device exists in this project (virtio9p.c), so unlike
// virtio.js's generic dev.ReceiveRequest/dev.SendReply callback
// object, this calls virtio9p_receive_request() directly -- no
// function-pointer indirection for a single fixed caller.
//
// Ring-buffer fields (descriptor table, avail/used rings) are
// little-endian on the wire regardless of the OR1000 CPU's
// big-endianness -- virtio is spec'd that way, and real guest drivers
// byte-swap explicitly. mem.c's ram_lw/ram_sw are big-endian
// (matching the CPU's own Read32Big), so virtio.c reads/writes ring
// memory one byte at a time via ram_lb/ram_sb and composes
// little-endian locally, matching Read16Little/Read32Little/
// Write16Little/Write32Little in ram.js exactly.

enum { VIRTIO_MMIO_BASE = 0x97000000 };
enum { VIRTIO_INTNO = 6 };

void virtio_reset();
int virtio_read8(int addr);
void virtio_write8(int addr, int val);
int virtio_read32(int addr);
void virtio_write32(int addr, int val);

// Streams request bytes out of a descriptor chain, starting at
// (queueidx, descindex) -- mirrors the JS GetByte closure in
// VirtIODev.prototype.WriteReg32's VIRTIO_QUEUENOTIFY_REG case.
// virtio9p.c calls virtio_stream_start once per request, then
// virtio_get_byte() for every byte of the request header + body.
void virtio_stream_start(int queueidx, int descindex);
int virtio_get_byte();

// Copies replybuffer[0..replybuffersize) into the request's
// write-only descriptor(s) and raises VIRTIO_INTNO -- matches
// VirtIODev.prototype.SendReply. virtio9p.c calls this once per
// request, after building its reply.
void virtio_send_reply(int queueidx, int descindex, char *replybuffer, int replybuffersize);
