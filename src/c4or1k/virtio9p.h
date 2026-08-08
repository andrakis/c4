// c4or1k's 9P2000.L protocol handler, matching
// jorconsole/jor1k/js/worker/dev/virtio/9p.js's Virtio9p op-for-op
// (same message IDs, same field layouts -- see that file's header
// comment for the protocol reference). Backed by bootfs.c's inode
// tree instead of jor1k's async, network-fed FS object; every op here
// completes synchronously and calls virtio_send_reply() itself before
// returning (jor1k's fs.AddEvent deferred-callback machinery has
// nothing to defer to in this project -- see bootfs.h's header
// comment).
//
// Wire values (9p message bodies) are little-endian, independent of
// the ring-buffer little-endian convention virtio.c handles and
// independent of the OR1000 CPU's big-endianness -- see marshall.js's
// header comment ("helper functions for virtio and 9p"). Request
// bytes are pulled one at a time via virtio_get_byte(); replies are
// built into a private buffer with a running cursor, then handed to
// virtio_send_reply() whole.

void virtio9p_init();

// Called by virtio.c once per available-ring entry, after
// virtio_stream_start(queueidx, descindex) has already been called
// for this request -- reads the request via virtio_get_byte(),
// dispatches on its 9p message id, and sends a reply itself.
void virtio9p_receive_request(int queueidx, int descindex);
