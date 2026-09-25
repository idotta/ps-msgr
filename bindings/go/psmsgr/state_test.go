// SPDX-License-Identifier: Apache-2.0

package psmsgr_test

import (
	"bytes"
	"context"
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"testing"
	"time"
	"unsafe"

	"github.com/ps-solucoes/ps-msgr/bindings/go/psmsgr"
)

// The C unit tests (tests/test_state.c), where relevant, through the binding.

func TestCreateThenReuse(t *testing.T) {
	c := newChannel(t)
	opts := psmsgr.WriterOptions{PayloadType: 0x1234, Mode: 0o640}
	w := c.writer(16, opts)
	for _, p := range []string{c.dataPath(), c.lockPath()} {
		if st := must[os.FileInfo](t)(os.Stat(p)); st.Mode().Perm() != 0o640 {
			t.Fatalf("%s: mode %v", p, st.Mode())
		}
	}
	if st := must[os.FileInfo](t)(os.Stat(c.dataPath())); st.Size() != 128+3*64 {
		t.Fatalf("size %d", st.Size())
	}
	if w.Capacity() != 16 {
		t.Fatalf("capacity %d", w.Capacity())
	}
	first := must[uint32](t)(w.Publish([]byte("one")))
	if first == 0 {
		t.Fatal("generation 0")
	}
	if gen := must[uint32](t)(w.Publish([]byte("two"))); gen != genAfter(first, 1) {
		t.Fatalf("generation %d after %d", gen, first)
	}
	must[uint32](t)(w.Publish([]byte("three")))
	ino := inode(t, c.dataPath())
	w.Close()

	w = c.writer(16, opts)
	if inode(t, c.dataPath()) != ino {
		t.Fatal("the channel file was replaced")
	}
	gen := must[uint32](t)(w.Publish([]byte("four")))
	if gen != genAfter(first, 3) {
		t.Fatalf("generation %d, want %d", gen, genAfter(first, 3))
	}
	r := c.reader()
	data, info, got, err := r.Read(nil)
	if err != nil || !got || string(data) != "four" {
		t.Fatalf("Read: %q %v %v", data, got, err)
	}
	peeked := ok[psmsgr.Info](t)(r.Peek())
	if want := (psmsgr.Info{Generation: gen, Length: 4, TimestampNs: peeked.TimestampNs, Attached: true}); info != want {
		t.Fatalf("info %+v, want %+v", info, want)
	}
	desc := ok[psmsgr.ChannelDesc](t)(r.Describe())
	if want := (psmsgr.ChannelDesc{Capacity: 16, SlotCount: 3, PayloadType: 0x1234, Notify: true}); desc != want {
		t.Fatalf("desc %+v, want %+v", desc, want)
	}
}

func TestMismatchAndRecreate(t *testing.T) {
	c := newChannel(t)
	w := c.writer(16, psmsgr.WriterOptions{})
	oldGen := must[uint32](t)(w.Publish([]byte("old")))
	w.Close()
	r := c.reader()
	if data, info, _, _ := r.Read(nil); string(data) != "old" || !info.Attached {
		t.Fatalf("Read: %q %+v", data, info)
	}

	for _, o := range []struct {
		capacity uint32
		opts     psmsgr.WriterOptions
	}{
		{32, psmsgr.WriterOptions{}},
		{16, psmsgr.WriterOptions{SlotCount: 4}},
		{16, psmsgr.WriterOptions{NoNotify: true}},
		{16, psmsgr.WriterOptions{PayloadType: 9}},
	} {
		_, err := c.open(o.capacity, o.opts)
		wantCode(t, err, psmsgr.ErrMismatch)
		if !errors.Is(err, psmsgr.ErrMismatch) {
			t.Fatalf("errors.Is(%v, ErrMismatch) is false", err)
		}
	}
	if info := ok[psmsgr.Info](t)(r.Peek()); info.Generation != oldGen {
		t.Fatalf("generation %d, want %d", info.Generation, oldGen)
	}

	ino := inode(t, c.dataPath())
	w = c.writer(32, psmsgr.WriterOptions{Recreate: true})
	if inode(t, c.dataPath()) == ino {
		t.Fatal("the channel file was not replaced")
	}
	if w.Capacity() != 32 {
		t.Fatalf("capacity %d", w.Capacity())
	}
	// The reader follows the retire to the new, still empty file.
	none[psmsgr.Info](t)(r.Peek())
	if desc := ok[psmsgr.ChannelDesc](t)(r.Describe()); desc.Capacity != 32 {
		t.Fatalf("capacity %d", desc.Capacity)
	}
	gen := must[uint32](t)(w.Publish([]byte("new")))
	if gen != genAfter(oldGen, 1) {
		t.Fatalf("generation %d, want %d", gen, genAfter(oldGen, 1))
	}
	if data, info, _, _ := r.Read(nil); string(data) != "new" || info.Generation != gen || !info.Attached {
		t.Fatalf("Read: %q %+v", data, info)
	}
	w.Close()

	// Recreate on a compatible file is a plain reuse.
	ino = inode(t, c.dataPath())
	c.writer(32, psmsgr.WriterOptions{Recreate: true}).Close()
	if inode(t, c.dataPath()) != ino {
		t.Fatal("the channel file was replaced")
	}
}

func TestSecondWriterSameProcess(t *testing.T) {
	c := newChannel(t)
	w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
	_, err := c.open(8, psmsgr.WriterOptions{SlotCount: 2})
	if e := wantCode(t, err, psmsgr.ErrWriterExists); e.Channel != chanName || e.Op != "open writer" {
		t.Fatalf("error %+v", e)
	}
	_, err = c.open(8, psmsgr.WriterOptions{SlotCount: 2, Recreate: true})
	wantCode(t, err, psmsgr.ErrWriterExists)
	_, err = psmsgr.Unlink(chanName, c.dir)
	wantCode(t, err, psmsgr.ErrWriterExists)
	w.Close()
	c.writer(8, psmsgr.WriterOptions{SlotCount: 2}).Close()
}

func TestSecondWriterOtherProcess(t *testing.T) {
	c := newChannel(t)
	r := c.reader()
	if must[bool](t)(r.WriterAlive()) {
		t.Fatal("alive without a lock file")
	}
	child := startChildWriter(t, c.dir, 5)
	_, err := c.open(24, psmsgr.WriterOptions{PayloadType: motorStatusV1})
	wantCode(t, err, psmsgr.ErrWriterExists)
	if !must[bool](t)(r.WriterAlive()) {
		t.Fatal("the child writer is not alive")
	}
	child.kill()
	if must[bool](t)(r.WriterAlive()) {
		t.Fatal("alive after SIGKILL")
	}
	var s motorStatus
	ok[psmsgr.Info](t)(psmsgr.ReadValue(r, &s))
	if s.Sequence != 5 || s.SpeedRpm != 2.5 || s.CurrentA != 5.0/64 || s.TemperatureC != 5.0/8-40 {
		t.Fatalf("status %+v", s)
	}
	w := c.writer(24, psmsgr.WriterOptions{PayloadType: motorStatusV1})
	if !must[bool](t)(r.WriterAlive()) {
		t.Fatal("not alive")
	}
	w.Close()
	if must[bool](t)(r.WriterAlive()) {
		t.Fatal("alive after Close")
	}
}

func TestLazyReaderAttachesLater(t *testing.T) {
	c := newChannel(t)
	r := c.reader()
	buf := make([]byte, 0, 16)
	data, info, got, err := r.Read(buf)
	if err != nil || got || info != (psmsgr.Info{}) || len(data) != 0 || cap(data) != 16 {
		t.Fatalf("Read: %q %+v %v %v", data, info, got, err)
	}
	var s motorStatus
	none[psmsgr.Info](t)(psmsgr.ReadValue(r, &s))
	none[psmsgr.Info](t)(r.Peek())
	none[psmsgr.ChannelDesc](t)(r.Describe())
	if must[bool](t)(r.Wait(context.Background(), 0, 0)) {
		t.Fatal("Wait returned true")
	}
	if must[bool](t)(r.WriterAlive()) {
		t.Fatal("alive")
	}

	w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
	none[psmsgr.Info](t)(r.Peek()) // attached, nothing published
	if desc := ok[psmsgr.ChannelDesc](t)(r.Describe()); desc.Capacity != 8 {
		t.Fatalf("capacity %d", desc.Capacity)
	}
	if !must[bool](t)(r.WriterAlive()) {
		t.Fatal("not alive")
	}
	gen := must[uint32](t)(w.Publish([]byte("hi")))
	data, info, got, err = r.Read(buf)
	if err != nil || !got || string(data) != "hi" || info.Generation != gen || info.Length != 2 || !info.Attached {
		t.Fatalf("Read: %q %+v %v %v", data, info, got, err)
	}
	if &data[0] != &buf[:1][0] {
		t.Fatal("Read allocated although buf had room")
	}
}

func TestReadAppends(t *testing.T) {
	c := newChannel(t)
	w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
	r := c.reader()
	_, err := w.Publish([]byte("123456789"))
	if e := wantCode(t, err, psmsgr.ErrTooBig); !strings.Contains(e.Error(), "9 bytes") {
		t.Fatalf("message %q", e.Error())
	}
	none[psmsgr.Info](t)(r.Peek()) // failures publish nothing
	gen := must[uint32](t)(w.Publish([]byte("12345678")))

	// Appended after the prefix; grown when the room is short.
	small := make([]byte, 2, 5)
	copy(small, "ab")
	data, info, _, err := r.Read(small)
	if err != nil || string(data) != "ab12345678" || string(small) != "ab" {
		t.Fatalf("Read: %q (%q) %v", data, small, err)
	}
	peeked := ok[psmsgr.Info](t)(r.Peek())
	if want := (psmsgr.Info{Generation: gen, Length: 8, TimestampNs: peeked.TimestampNs, Attached: true}); info != want {
		t.Fatalf("info %+v, want %+v", info, want)
	}
	// Into the room of the returned slice, without allocating.
	data2, info, _, _ := r.Read(data[:0])
	if string(data2) != "12345678" || &data2[0] != &data[0] || info.Attached {
		t.Fatalf("Read: %q %+v", data2, info)
	}

	must[uint32](t)(w.Publish(nil))
	data, info, got, err := r.Read(nil)
	if err != nil || !got || len(data) != 0 || info.Length != 0 {
		t.Fatalf("Read: %q %+v %v %v", data, info, got, err)
	}
}

func TestValues(t *testing.T) {
	if unsafe.Sizeof(motorStatus{}) != 24 {
		t.Fatalf("sizeof motorStatus %d", unsafe.Sizeof(motorStatus{}))
	}
	c := newChannel(t)
	w := c.writer(24, psmsgr.WriterOptions{PayloadType: motorStatusV1})
	r := c.reader()
	status := motorStatus{Sequence: 1, SpeedRpm: 1500, CurrentA: 2.5, TemperatureC: 41}
	gen := must[uint32](t)(psmsgr.PublishValue(w, &status))
	var got motorStatus
	info := ok[psmsgr.Info](t)(psmsgr.ReadValue(r, &got))
	if got != status || info.Generation != gen || info.Length != 24 || !info.Attached {
		t.Fatalf("ReadValue: %+v %+v", got, info)
	}
	// PublishValue writes the same bytes as Publish would.
	data, _, _, _ := r.Read(nil)
	if !bytes.Equal(data, unsafe.Slice((*byte)(unsafe.Pointer(&status)), 24)) {
		t.Fatalf("bytes %x", data)
	}

	// A payload of another length doesn't fit the type, and the attach is
	// reported later.
	must[uint32](t)(w.Publish([]byte("short")))
	r2 := c.reader()
	_, _, err := psmsgr.ReadValue(r2, &got)
	if e := wantCode(t, err, psmsgr.ErrMismatch); !strings.Contains(e.Error(), "5 bytes") {
		t.Fatalf("message %q", e.Error())
	}
	if info := ok[psmsgr.Info](t)(r2.Peek()); !info.Attached {
		t.Fatal("the attach was lost")
	}
	must[uint32](t)(w.Publish(make([]byte, 24)))
	var small int64
	_, _, err = psmsgr.ReadValue(r, &small)
	wantCode(t, err, psmsgr.ErrMismatch)
	info = ok[psmsgr.Info](t)(psmsgr.ReadValue(r, &got))
	if got != (motorStatus{}) || info.Attached {
		t.Fatalf("ReadValue: %+v %+v", got, info)
	}

	// Too big for the channel.
	var big [25]byte
	_, err = psmsgr.PublishValue(w, &big)
	wantCode(t, err, psmsgr.ErrTooBig)

	// Types with pointers are refused, before anything is written.
	type withString struct{ S string }
	type withSlice struct{ A [2][]byte }
	type withPointer struct {
		N int32
		P *int32
	}
	var ws withString
	_, err = psmsgr.PublishValue(w, &ws)
	wantCode(t, err, psmsgr.ErrInval)
	_, _, err = psmsgr.ReadValue(r, &ws)
	wantCode(t, err, psmsgr.ErrInval)
	_, _, err = psmsgr.ReadValue(r, &withSlice{})
	wantCode(t, err, psmsgr.ErrInval)
	_, err = psmsgr.PublishValue(w, &withPointer{})
	if e := wantCode(t, err, psmsgr.ErrInval); !strings.Contains(e.Error(), "withPointer holds pointers") {
		t.Fatalf("message %q", e.Error())
	}
	// Arrays and nested structs of plain data are fine.
	type nested struct {
		A [3]uint16
		B struct{ X, Y float32 }
		C bool
		_ [3]byte
	}
	v := nested{A: [3]uint16{1, 2, 3}, C: true}
	v.B.X = 4
	must[uint32](t)(psmsgr.PublishValue(w, &v))
	var v2 nested
	ok[psmsgr.Info](t)(psmsgr.ReadValue(r, &v2))
	if v2 != v {
		t.Fatalf("ReadValue: %+v", v2)
	}
}

func TestHeartbeatChannel(t *testing.T) {
	c := newChannel(t)
	w := c.writer(0, psmsgr.WriterOptions{SlotCount: 2})
	if w.Capacity() != 0 {
		t.Fatalf("capacity %d", w.Capacity())
	}
	if st := must[os.FileInfo](t)(os.Stat(c.dataPath())); st.Size() != 128+2*64 {
		t.Fatalf("size %d", st.Size())
	}
	_, err := w.Publish([]byte("x"))
	wantCode(t, err, psmsgr.ErrTooBig)
	first := must[uint32](t)(w.Publish(nil))
	gen := must[uint32](t)(w.Publish([]byte{}))
	if gen != genAfter(first, 1) {
		t.Fatalf("generation %d", gen)
	}
	r := c.reader()
	data, info, got, err := r.Read(nil)
	if err != nil || !got || len(data) != 0 || info.Generation != gen {
		t.Fatalf("Read: %q %+v %v %v", data, info, got, err)
	}
	gen2 := must[uint32](t)(w.PublishFunc(func(buf []byte) (int, error) {
		if len(buf) != 0 {
			t.Errorf("buffer of %d bytes", len(buf))
		}
		return 0, nil
	}))
	if gen2 != genAfter(gen, 1) {
		t.Fatalf("generation %d", gen2)
	}
}

func TestPeekTimestamps(t *testing.T) {
	c := newChannel(t)
	w := c.writer(8, psmsgr.WriterOptions{})
	r := c.reader()
	var prev uint64
	var first uint32
	for i := 1; i <= 20; i++ {
		before := psmsgr.NowNs()
		gen := must[uint32](t)(w.Publish([]byte{byte(i), 0, 0, 0}))
		after := psmsgr.NowNs()
		if i == 1 {
			first = gen
		}
		if gen != genAfter(first, i-1) {
			t.Fatalf("generation %d", gen)
		}
		info := ok[psmsgr.Info](t)(r.Peek())
		if info.Generation != gen || info.Length != 4 || info.Attached != (i == 1) {
			t.Fatalf("info %+v", info)
		}
		if info.TimestampNs < before || info.TimestampNs > after || info.TimestampNs < prev {
			t.Fatalf("timestamp %d not in [%d, %d] or before %d", info.TimestampNs, before, after, prev)
		}
		if age := info.Age(); age < 0 || age > 10*time.Second {
			t.Fatalf("age %v", age)
		}
		prev = info.TimestampNs
	}
}

func TestWaitWakesOnPublish(t *testing.T) {
	c := newChannel(t)
	w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
	r := c.reader()
	ctx := context.Background()
	gen := must[uint32](t)(w.Publish([]byte("a")))
	// A value that differs from lastGeneration returns at once.
	if !must[bool](t)(r.Wait(ctx, 0, 0)) || !must[bool](t)(r.Wait(ctx, genAfter(gen, 1), psmsgr.NoTimeout)) {
		t.Fatal("Wait returned false")
	}

	done := make(chan error, 1)
	go func() {
		changed, err := r.Wait(ctx, gen, 10*time.Second)
		if err == nil && !changed {
			err = errors.New("timed out")
		}
		done <- err
	}()
	time.Sleep(50 * time.Millisecond) // likely blocked by now; the test holds either way
	must[uint32](t)(w.Publish([]byte("b")))
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	if info := ok[psmsgr.Info](t)(r.Peek()); info.Generation != genAfter(gen, 1) {
		t.Fatalf("generation %d", info.Generation)
	}
}

func TestWaitTimeouts(t *testing.T) {
	c := newChannel(t)
	w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
	r := c.reader()
	ctx := context.Background()
	if must[bool](t)(r.Wait(ctx, 0, 0)) { // nothing published counts as unchanged
		t.Fatal("Wait returned true")
	}
	gen := must[uint32](t)(w.Publish([]byte("a")))
	if must[bool](t)(r.Wait(ctx, gen, 0)) {
		t.Fatal("Wait returned true")
	}
	for _, timeout := range []time.Duration{120 * time.Millisecond, time.Nanosecond} {
		start := time.Now()
		if must[bool](t)(r.Wait(ctx, gen, timeout)) {
			t.Fatal("Wait returned true")
		}
		// Below a millisecond: rounded up, not a poll.
		if elapsed := time.Since(start); elapsed < max(timeout, time.Millisecond) {
			t.Fatalf("Wait(%v) returned after %v", timeout, elapsed)
		}
	}
	absent := must[*psmsgr.Reader](t)(psmsgr.OpenReader("absent", c.dir))
	defer absent.Close()
	start := time.Now()
	if must[bool](t)(absent.Wait(ctx, 0, 50*time.Millisecond)) || time.Since(start) < 50*time.Millisecond {
		t.Fatal("Wait on an absent channel")
	}

	// The longest timeout still waits (for a publish here).
	done := make(chan bool, 1)
	go func() { changed, _ := r.Wait(ctx, gen, time.Duration(1<<63-1)); done <- changed }()
	time.Sleep(50 * time.Millisecond)
	must[uint32](t)(w.Publish([]byte("b")))
	select {
	case changed := <-done:
		if !changed {
			t.Fatal("Wait returned false")
		}
	case <-time.After(10 * time.Second):
		t.Fatal("Wait did not return")
	}
}

func TestWaitContext(t *testing.T) {
	c := newChannel(t)
	w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
	r := c.reader()
	gen := must[uint32](t)(w.Publish([]byte("a")))

	canceled, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := r.Wait(canceled, 0, 0); !errors.Is(err, context.Canceled) {
		t.Fatalf("Wait: %v", err)
	}

	// A deadline ends the wait on time, with any timeout.
	for _, timeout := range []time.Duration{psmsgr.NoTimeout, 30 * time.Second} {
		// start before the deadline is set, or the deadline can fall short
		// of 150 ms after it.
		start := time.Now()
		ctx, cancel := context.WithTimeout(context.Background(), 150*time.Millisecond)
		_, err := r.Wait(ctx, gen, timeout)
		elapsed := time.Since(start)
		cancel()
		if !errors.Is(err, context.DeadlineExceeded) {
			t.Fatalf("Wait: %v", err)
		}
		if elapsed < 150*time.Millisecond || elapsed > 2*time.Second {
			t.Fatalf("Wait returned after %v", elapsed)
		}
	}

	// A cancel from another goroutine stops it within a slice.
	ctx, cancel := context.WithCancel(context.Background())
	start := time.Now()
	time.AfterFunc(150*time.Millisecond, cancel)
	if _, err := r.Wait(ctx, gen, psmsgr.NoTimeout); !errors.Is(err, context.Canceled) {
		t.Fatalf("Wait: %v", err)
	}
	if elapsed := time.Since(start); elapsed < 150*time.Millisecond || elapsed > 2*time.Second {
		t.Fatalf("Wait returned after %v", elapsed)
	}

	// A cancelable wait still ends on its timeout, and on a publish.
	ctx, cancel = context.WithCancel(context.Background())
	defer cancel()
	start = time.Now()
	if must[bool](t)(r.Wait(ctx, gen, 250*time.Millisecond)) || time.Since(start) < 250*time.Millisecond {
		t.Fatal("Wait did not time out")
	}
	time.AfterFunc(150*time.Millisecond, func() { w.Publish([]byte("b")) })
	if !must[bool](t)(r.Wait(ctx, gen, psmsgr.NoTimeout)) {
		t.Fatal("Wait returned false")
	}
}

func TestCloseStopsWait(t *testing.T) {
	c := newChannel(t)
	w := c.writer(8, psmsgr.WriterOptions{})
	gen := must[uint32](t)(w.Publish([]byte("a")))
	for _, timeout := range []time.Duration{psmsgr.NoTimeout, 30 * time.Second} {
		r := must[*psmsgr.Reader](t)(psmsgr.OpenReader(chanName, c.dir))
		done := make(chan error, 1)
		go func() {
			_, err := r.Wait(context.Background(), gen, timeout)
			done <- err
		}()
		time.Sleep(50 * time.Millisecond) // likely blocked by now; the test holds either way
		start := time.Now()
		r.Close()
		select {
		case err := <-done:
			if !errors.Is(err, psmsgr.ErrClosed) {
				t.Fatalf("Wait: %v", err)
			}
		case <-time.After(10 * time.Second):
			t.Fatal("Wait did not return")
		}
		if elapsed := time.Since(start); elapsed > 2*time.Second {
			t.Fatalf("Wait returned after %v", elapsed)
		}
		if _, err := r.WriterAlive(); !errors.Is(err, psmsgr.ErrClosed) {
			t.Fatalf("WriterAlive: %v", err)
		}
		if _, err := r.Wait(context.Background(), gen, 0); !errors.Is(err, psmsgr.ErrClosed) {
			t.Fatalf("Wait: %v", err)
		}
	}
}

func TestWaitFollowsRetire(t *testing.T) {
	c := newChannel(t)
	r := c.reader()
	w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
	gen := must[uint32](t)(w.Publish([]byte("a")))
	w.Close()
	if !must[bool](t)(r.Wait(context.Background(), 0, 0)) { // attach
		t.Fatal("Wait returned false")
	}
	done := make(chan bool, 1)
	go func() { changed, _ := r.Wait(context.Background(), gen, 10*time.Second); done <- changed }()
	time.Sleep(50 * time.Millisecond)
	w = c.writer(16, psmsgr.WriterOptions{SlotCount: 2, Recreate: true})
	time.Sleep(50 * time.Millisecond)
	select {
	case <-done:
		t.Fatal("the retire alone returned")
	default:
	}
	must[uint32](t)(w.Publish([]byte("b")))
	if !<-done {
		t.Fatal("Wait returned false")
	}
	if desc := ok[psmsgr.ChannelDesc](t)(r.Describe()); desc.Capacity != 16 {
		t.Fatalf("capacity %d", desc.Capacity)
	}
}

func TestWaitFollowsUnlink(t *testing.T) {
	c := newChannel(t)
	r := c.reader()
	w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
	gen := must[uint32](t)(w.Publish([]byte("a")))
	w.Close()
	if !must[bool](t)(r.Wait(context.Background(), 0, 0)) {
		t.Fatal("Wait returned false")
	}
	done := make(chan bool, 1)
	go func() { changed, _ := r.Wait(context.Background(), gen, 10*time.Second); done <- changed }()
	time.Sleep(50 * time.Millisecond)
	if !must[bool](t)(psmsgr.Unlink(chanName, c.dir)) {
		t.Fatal("Unlink returned false")
	}
	for _, p := range []string{c.dataPath(), c.lockPath()} {
		if _, err := os.Stat(p); !errors.Is(err, fs.ErrNotExist) {
			t.Fatalf("%s: %v", p, err)
		}
	}
	time.Sleep(50 * time.Millisecond)
	select {
	case <-done:
		t.Fatal("the unlink alone returned")
	default:
	}
	w = c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
	newGen := must[uint32](t)(w.Publish([]byte("c"))) // must not reuse gen
	if !<-done {
		t.Fatal("Wait returned false")
	}
	w.Close()
	if info := ok[psmsgr.Info](t)(r.Peek()); info.Generation != newGen {
		t.Fatalf("generation %d, want %d", info.Generation, newGen)
	}
	if !must[bool](t)(psmsgr.Unlink(chanName, c.dir)) {
		t.Fatal("Unlink returned false")
	}
	none[psmsgr.Info](t)(r.Peek())
	if must[bool](t)(psmsgr.Unlink(chanName, c.dir)) {
		t.Fatal("Unlink returned true")
	}
}

func TestWaitNotSupportedWithoutNotify(t *testing.T) {
	c := newChannel(t)
	w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2, NoNotify: true})
	r := c.reader()
	must[uint32](t)(w.Publish([]byte("a")))
	_, err := r.Wait(context.Background(), 0, 100*time.Millisecond)
	wantCode(t, err, psmsgr.ErrNotSup)
	if desc := ok[psmsgr.ChannelDesc](t)(r.Describe()); desc != (psmsgr.ChannelDesc{Capacity: 8, SlotCount: 2}) {
		t.Fatalf("desc %+v", desc)
	}
}

func TestAttachedOncePerAttach(t *testing.T) {
	c := newChannel(t)
	r := c.reader()
	w := c.writer(8, psmsgr.WriterOptions{SlotCount: 2})
	must[uint32](t)(w.Publish(nil))
	ok[psmsgr.ChannelDesc](t)(r.Describe())                 // attaches, does not consume
	if !must[bool](t)(r.Wait(context.Background(), 0, 0)) { // neither does wait
		t.Fatal("Wait returned false")
	}
	if !ok[psmsgr.Info](t)(r.Peek()).Attached || ok[psmsgr.Info](t)(r.Peek()).Attached {
		t.Fatal("attached not reported exactly once")
	}
	if _, info, _, _ := r.Read(nil); info.Attached {
		t.Fatal("attached reported twice")
	}
	w.Close()
	w = c.writer(8, psmsgr.WriterOptions{SlotCount: 3, Recreate: true})
	none[psmsgr.Info](t)(r.Peek())
	must[uint32](t)(w.Publish(nil))
	if _, info, _, _ := r.Read(nil); !info.Attached {
		t.Fatal("attached not reported")
	}
	if ok[psmsgr.Info](t)(r.Peek()).Attached {
		t.Fatal("attached reported twice")
	}
}

func TestPublishFunc(t *testing.T) {
	c := newChannel(t)
	w := c.writer(16, psmsgr.WriterOptions{})
	r := c.reader()
	gen := must[uint32](t)(w.PublishFunc(func(buf []byte) (int, error) {
		if len(buf) != 16 || uintptr(unsafe.Pointer(&buf[0]))%32 != 0 {
			t.Errorf("buffer of %d bytes at %p", len(buf), &buf[0])
		}
		// A publish or a nested PublishFunc while building.
		_, err := w.Publish([]byte("x"))
		wantCode(t, err, psmsgr.ErrState)
		_, err = w.PublishFunc(func([]byte) (int, error) { return 0, nil })
		if e := wantCode(t, err, psmsgr.ErrState); e.Op != "begin" {
			t.Errorf("op %q", e.Op)
		}
		return copy(buf, "zero-copy"), nil
	}))
	if data, info, _, _ := r.Read(nil); string(data) != "zero-copy" || info.Generation != gen {
		t.Fatalf("Read: %q %+v", data, info)
	}

	// Every failure aborts: readers keep the last value.
	errBuild := errors.New("build failed")
	failures := []func([]byte) (int, error){
		func(buf []byte) (int, error) { copy(buf, "partial"); return 0, errBuild },
		func(buf []byte) (int, error) { copy(buf, "partial"); return 17, nil },
		func(buf []byte) (int, error) { copy(buf, "partial"); return -1, nil },
	}
	for i, build := range failures {
		_, err := w.PublishFunc(build)
		switch i {
		case 0:
			if err != errBuild {
				t.Fatalf("PublishFunc: %v", err)
			}
		case 1:
			wantCode(t, err, psmsgr.ErrTooBig)
		case 2:
			wantCode(t, err, psmsgr.ErrInval)
		}
		if data, info, _, _ := r.Read(nil); string(data) != "zero-copy" || info.Generation != gen {
			t.Fatalf("Read: %q %+v", data, info)
		}
	}
	func() {
		defer func() {
			if recover() != "boom" {
				t.Fatal("the panic was lost")
			}
		}()
		w.PublishFunc(func(buf []byte) (int, error) { copy(buf, "partial"); panic("boom") })
	}()
	if data, _, _, _ := r.Read(nil); string(data) != "zero-copy" {
		t.Fatalf("Read: %q", data)
	}
	// The writer is usable after each of them.
	gen = must[uint32](t)(w.PublishFunc(func(buf []byte) (int, error) { return copy(buf, "sixteen bytes!!!"), nil }))
	if data, info, _, _ := r.Read(nil); string(data) != "sixteen bytes!!!" || info.Generation != gen {
		t.Fatalf("Read: %q %+v", data, info)
	}

	// Closing the writer while building aborts.
	_, err := w.PublishFunc(func(buf []byte) (int, error) { w.Close(); return 0, nil })
	if !errors.Is(err, psmsgr.ErrClosed) {
		t.Fatalf("PublishFunc: %v", err)
	}
	if data, _, _, _ := r.Read(nil); string(data) != "sixteen bytes!!!" {
		t.Fatalf("Read: %q", data)
	}
}

func TestUnlink(t *testing.T) {
	c := newChannel(t)
	if must[bool](t)(psmsgr.Unlink(chanName, c.dir)) {
		t.Fatal("Unlink returned true")
	}
	w := c.writer(8, psmsgr.WriterOptions{})
	_, err := psmsgr.Unlink(chanName, c.dir)
	wantCode(t, err, psmsgr.ErrWriterExists)
	w.Close()
	if !must[bool](t)(psmsgr.Unlink(chanName, c.dir)) {
		t.Fatal("Unlink returned false")
	}
	for _, p := range []string{c.dataPath(), c.lockPath()} {
		if _, err := os.Stat(p); !errors.Is(err, fs.ErrNotExist) {
			t.Fatalf("%s: %v", p, err)
		}
	}
}

func TestInvalidArguments(t *testing.T) {
	c := newChannel(t)
	for _, name := range []string{"", ".x", "a/b", "a b", "ä", strings.Repeat("n", 65), "a\x00b"} {
		_, err := psmsgr.OpenReader(name, c.dir)
		wantCode(t, err, psmsgr.ErrInval)
		_, err = psmsgr.OpenWriter(name, 8, &psmsgr.WriterOptions{Dir: c.dir})
		wantCode(t, err, psmsgr.ErrInval)
		_, err = psmsgr.Unlink(name, c.dir)
		if e := wantCode(t, err, psmsgr.ErrInval); e.Errno != 0 {
			t.Fatalf("errno %d", e.Errno)
		}
	}
	must[*psmsgr.Reader](t)(psmsgr.OpenReader(strings.Repeat("n", 64), c.dir)).Close()

	_, err := psmsgr.Unlink(chanName, c.dir+"\x00x")
	if e := wantCode(t, err, psmsgr.ErrInval); !strings.Contains(e.Error(), "directory contains a NUL") {
		t.Fatalf("message %q", e.Error())
	}
	_, err = psmsgr.OpenReader(chanName, c.dir+"\x00x")
	wantCode(t, err, psmsgr.ErrInval)
	_, err = psmsgr.OpenWriter(chanName, 8, &psmsgr.WriterOptions{Dir: c.dir + "\x00x"})
	wantCode(t, err, psmsgr.ErrInval)

	for _, o := range []struct {
		capacity uint32
		opts     psmsgr.WriterOptions
	}{
		{8, psmsgr.WriterOptions{SlotCount: 1}},
		{8, psmsgr.WriterOptions{SlotCount: 17}},
		{8, psmsgr.WriterOptions{Mode: 0o10644}},
		{16<<20 + 1, psmsgr.WriterOptions{}},
	} {
		_, err := c.open(o.capacity, o.opts)
		wantCode(t, err, psmsgr.ErrInval)
	}
	if _, err := os.Stat(c.lockPath()); !errors.Is(err, fs.ErrNotExist) {
		t.Fatal("something was created")
	}
}

func TestSystemErrors(t *testing.T) {
	c := newChannel(t)
	_, err := psmsgr.OpenWriter(chanName, 8, &psmsgr.WriterOptions{Dir: filepath.Join(c.dir, "missing")})
	e := wantCode(t, err, psmsgr.ErrSys)
	if e.Errno != syscall.ENOENT || !errors.Is(err, fs.ErrNotExist) || !errors.Is(err, psmsgr.ErrSys) {
		t.Fatalf("error %+v", e)
	}
	if e.Error() != `psmsgr: open writer "chan": no such file or directory` {
		t.Fatalf("message %q", e.Error())
	}

	if err := os.Symlink(filepath.Join(c.dir, "elsewhere"), c.dataPath()); err != nil {
		t.Fatal(err)
	}
	_, err = c.open(8, psmsgr.WriterOptions{})
	if e := wantCode(t, err, psmsgr.ErrSys); e.Errno != syscall.ELOOP {
		t.Fatalf("errno %v", e.Errno)
	}
	r := c.reader()
	_, _, err = r.Peek()
	if e := wantCode(t, err, psmsgr.ErrSys); e.Errno != syscall.ELOOP || e.Op != "peek" {
		t.Fatalf("error %+v", e)
	}
}

func TestFormatError(t *testing.T) {
	c := newChannel(t)
	if err := os.WriteFile(c.dataPath(), bytes.Repeat([]byte("not a channel"), 20), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err := c.open(8, psmsgr.WriterOptions{})
	wantCode(t, err, psmsgr.ErrFormat)
	r := c.reader()
	_, _, _, err = r.Read(nil)
	wantCode(t, err, psmsgr.ErrFormat)
	c.writer(8, psmsgr.WriterOptions{Recreate: true}).Close()
}

func TestErrors(t *testing.T) {
	for _, c := range []struct {
		err  error
		want string
	}{
		{&psmsgr.Error{Op: "read", Channel: "chan", Code: psmsgr.ErrBusy}, `psmsgr: read "chan": channel busy`},
		{&psmsgr.Error{Op: "open reader", Channel: "chan", Code: psmsgr.ErrSys, Errno: syscall.EACCES},
			`psmsgr: open reader "chan": permission denied`},
		{&psmsgr.Error{Op: "load", Code: -99}, "psmsgr: load: unknown error"},
		{&psmsgr.Error{Op: "x", Code: psmsgr.ErrInval, Msg: "custom"}, "psmsgr: x: custom"},
		{psmsgr.ErrWriterExists, "another writer holds the channel"},
	} {
		if got := c.err.Error(); got != c.want {
			t.Errorf("got %q, want %q", got, c.want)
		}
	}
	err := error(&psmsgr.Error{Op: "open reader", Code: psmsgr.ErrSys, Errno: syscall.EACCES})
	if !errors.Is(err, fs.ErrPermission) || !errors.Is(err, psmsgr.ErrSys) || errors.Is(err, psmsgr.ErrBusy) {
		t.Fatal("errors.Is")
	}
	if errors.Unwrap(&psmsgr.Error{Code: psmsgr.ErrBusy}) != nil {
		t.Fatal("Unwrap without an errno")
	}
}

func TestUseAfterClose(t *testing.T) {
	c := newChannel(t)
	w := c.writer(8, psmsgr.WriterOptions{})
	r := c.reader()
	for range 2 {
		if w.Close() != nil || r.Close() != nil {
			t.Fatal("Close failed")
		}
	}
	var v int32
	calls := map[string]func() error{
		"Publish":      func() error { _, err := w.Publish(nil); return err },
		"Publish big":  func() error { _, err := w.Publish(make([]byte, 9)); return err },
		"PublishValue": func() error { _, err := psmsgr.PublishValue(w, &v); return err },
		"PublishFunc":  func() error { _, err := w.PublishFunc(nil); return err },
		"Read":         func() error { _, _, _, err := r.Read(nil); return err },
		"ReadValue":    func() error { _, _, err := psmsgr.ReadValue(r, &v); return err },
		"Peek":         func() error { _, _, err := r.Peek(); return err },
		"Wait":         func() error { _, err := r.Wait(context.Background(), 0, 0); return err },
		"WriterAlive":  func() error { _, err := r.WriterAlive(); return err },
		"Describe":     func() error { _, _, err := r.Describe(); return err },
	}
	for name, call := range calls {
		if err := call(); !errors.Is(err, psmsgr.ErrClosed) {
			t.Errorf("%s: %v", name, err)
		}
	}
	if w.Capacity() != 8 {
		t.Fatal("Capacity after Close")
	}
}

// A handle that is garbage collected without Close is closed: the writer
// lock is released.
func TestCleanupClosesForgottenHandles(t *testing.T) {
	c := newChannel(t)
	func() {
		o := psmsgr.WriterOptions{Dir: c.dir}
		must[*psmsgr.Writer](t)(psmsgr.OpenWriter(chanName, 8, &o))
		must[*psmsgr.Reader](t)(psmsgr.OpenReader(chanName, c.dir))
	}()
	deadline := time.Now().Add(10 * time.Second)
	for {
		runtime.GC()
		w, err := c.open(8, psmsgr.WriterOptions{})
		if err == nil {
			w.Close()
			return
		}
		wantCode(t, err, psmsgr.ErrWriterExists)
		if time.Now().After(deadline) {
			t.Fatal("the forgotten writer still holds the lock")
		}
		time.Sleep(10 * time.Millisecond)
	}
}
