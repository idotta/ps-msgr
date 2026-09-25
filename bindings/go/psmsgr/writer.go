// SPDX-License-Identifier: Apache-2.0

package psmsgr

// #cgo noescape psmsgr_state_options_init_sized
// #cgo nocallback psmsgr_state_options_init_sized
// #cgo noescape psmsgr_state_writer_open
// #cgo nocallback psmsgr_state_writer_open
// #cgo nocallback psmsgr_state_writer_close
// #cgo nocallback psmsgr_state_writer_capacity
// #cgo noescape psmsgr_state_publish
// #cgo nocallback psmsgr_state_publish
// #cgo noescape psmsgr_state_begin
// #cgo nocallback psmsgr_state_begin
// #cgo noescape psmsgr_state_commit
// #cgo nocallback psmsgr_state_commit
// #cgo nocallback psmsgr_state_abort
// #include <stdlib.h>
// #include <psmsgr/psmsgr.h>
import "C"

import (
	"runtime"
	"strconv"
	"unsafe"
)

// WriterOptions are the channel's properties for [OpenWriter]. The zero
// value is the library's defaults.
type WriterOptions struct {
	SlotCount   uint32 // 2..16; 0: 3
	PayloadType uint32 // application tag, e.g. schema ID and version
	Mode        uint32 // file mode, e.g. 0o640; 0: 0o644
	Recreate    bool   // replace a channel with other geometry instead of ErrMismatch
	NoNotify    bool   // no futex wake per publish; Reader.Wait then fails with ErrNotSup
	Dir         string // "": $PSMSGR_DIR, else /dev/shm
}

// Writer is the single writer of a channel. It is not safe for concurrent
// use.
type Writer struct {
	h        *C.psmsgr_state_writer
	name     string
	capacity uint32
	cleanup  runtime.Cleanup
}

// OpenWriter opens or creates the channel name with room for payloads of up
// to capacity bytes (0 .. 16 MiB), and takes the writer lock. opts nil means
// the defaults. It fails with ErrWriterExists while another writer holds the
// channel, and with ErrMismatch if the channel exists with another capacity,
// slot count, payload type or notification setting (unless Recreate).
func OpenWriter(name string, capacity uint32, opts *WriterOptions) (*Writer, error) {
	const op = "open writer"
	if err := checkLoaded(); err != nil {
		return nil, err
	}
	var o WriterOptions
	if opts != nil {
		o = *opts
	}
	cname, err := cString(op, name, name, "name")
	if err != nil {
		return nil, err
	}
	defer C.free(unsafe.Pointer(cname))
	cdir, err := cDir(op, name, o.Dir)
	if err != nil {
		return nil, err
	}
	defer C.free(unsafe.Pointer(cdir))

	var co C.psmsgr_state_options
	C.psmsgr_state_options_init_sized(&co, C.uint32_t(unsafe.Sizeof(co)))
	co.capacity = C.uint32_t(capacity)
	if o.SlotCount != 0 {
		co.slot_count = C.uint32_t(o.SlotCount)
	}
	co.payload_type = C.uint32_t(o.PayloadType)
	if o.Mode != 0 {
		co.mode = C.uint32_t(o.Mode)
	}
	if o.Recreate {
		co.flags |= C.PSMSGR_STATE_RECREATE
	}
	if o.NoNotify {
		co.flags |= C.PSMSGR_STATE_NO_NOTIFY
	}
	co.dir = cdir

	var h *C.psmsgr_state_writer
	rc, errno := C.psmsgr_state_writer_open(cname, &co, &h)
	if rc != C.PSMSGR_OK {
		return nil, newError(op, name, rc, errno)
	}
	w := &Writer{h: h, name: name, capacity: uint32(C.psmsgr_state_writer_capacity(h))}
	w.cleanup = runtime.AddCleanup(w, closeWriter, h)
	return w, nil
}

func closeWriter(h *C.psmsgr_state_writer) { C.psmsgr_state_writer_close(h) }

// Capacity is the largest payload in bytes.
func (w *Writer) Capacity() uint32 { return w.capacity }

// Publish copies data into the channel and returns its generation. It fails
// with ErrTooBig if data is longer than the capacity.
func (w *Writer) Publish(data []byte) (uint32, error) {
	if uint64(len(data)) > uint64(w.capacity) {
		if w.h == nil {
			return 0, ErrClosed
		}
		return 0, w.tooBig("publish", len(data))
	}
	return w.publish(unsafe.Pointer(unsafe.SliceData(data)), C.uint32_t(len(data)))
}

func (w *Writer) publish(data unsafe.Pointer, n C.uint32_t) (uint32, error) {
	if w.h == nil {
		return 0, ErrClosed
	}
	var gen C.uint32_t
	rc := C.psmsgr_state_publish(w.h, data, n, &gen)
	runtime.KeepAlive(w)
	if rc != C.PSMSGR_OK {
		return 0, newError("publish", w.name, rc, nil)
	}
	return uint32(gen), nil
}

// PublishFunc publishes a value that build writes in place, without a copy.
// build gets the slot to write into, Capacity bytes (32-byte aligned), and
// returns the value's length. If build returns an error or panics, or the
// length is out of range, readers keep the previous value. buf is shared
// memory: it must not be used after build returns.
//
// It saves Publish's copy only when the value is built in buf; a value that
// is already in a buffer of its own gains nothing. On the BeagleBone Black
// the saving is negligible up to 256 B and about half at 64 KiB.
func (w *Writer) PublishFunc(build func(buf []byte) (int, error)) (uint32, error) {
	if w.h == nil {
		return 0, ErrClosed
	}
	var p unsafe.Pointer
	if rc := C.psmsgr_state_begin(w.h, &p); rc != C.PSMSGR_OK {
		return 0, newError("begin", w.name, rc, nil)
	}
	committed := false
	defer func() {
		// Close aborts an open begin itself.
		if !committed && w.h != nil {
			C.psmsgr_state_abort(w.h)
		}
		runtime.KeepAlive(w)
	}()
	n, err := build(unsafe.Slice((*byte)(p), w.capacity))
	if err != nil {
		return 0, err
	}
	if w.h == nil { // build closed the writer
		return 0, ErrClosed
	}
	if n < 0 {
		return 0, &Error{Op: "commit", Channel: w.name, Code: ErrInval, Msg: "negative length"}
	}
	if uint64(n) > uint64(w.capacity) {
		return 0, w.tooBig("commit", n)
	}
	var gen C.uint32_t
	if rc := C.psmsgr_state_commit(w.h, C.uint32_t(n), &gen); rc != C.PSMSGR_OK {
		return 0, newError("commit", w.name, rc, nil)
	}
	committed = true
	return uint32(gen), nil
}

func (w *Writer) tooBig(op string, n int) error {
	return &Error{Op: op, Channel: w.name, Code: ErrTooBig,
		Msg: "payload of " + strconv.Itoa(n) + " bytes exceeds the capacity of " + strconv.FormatUint(uint64(w.capacity), 10)}
}

// Close releases the writer lock; the channel and its last value remain.
// It is idempotent and returns nil.
func (w *Writer) Close() error {
	if w.h == nil {
		return nil
	}
	w.cleanup.Stop()
	C.psmsgr_state_writer_close(w.h)
	w.h = nil
	return nil
}
