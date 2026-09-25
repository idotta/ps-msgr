// SPDX-License-Identifier: Apache-2.0

package psmsgr_test

import (
	"testing"

	"github.com/ps-solucoes/ps-msgr/bindings/go/psmsgr"
)

// The hot path allocates nothing: publishing, reading into a reused buffer
// or a value, peeking.
func TestHotPathDoesNotAllocate(t *testing.T) {
	if raceEnabled {
		t.Skip("the race detector allocates")
	}
	c := newChannel(t)
	w := c.writer(64, psmsgr.WriterOptions{})
	r := c.reader()
	data := make([]byte, 24)
	buf := make([]byte, 0, 64)
	var status motorStatus
	for name, f := range map[string]func(){
		"Publish":      func() { w.Publish(data) },
		"PublishValue": func() { psmsgr.PublishValue(w, &status) },
		"PublishFunc":  func() { w.PublishFunc(func(b []byte) (int, error) { return copy(b, data), nil }) },
		"Read":         func() { buf, _, _, _ = r.Read(buf[:0]) },
		"ReadValue":    func() { psmsgr.ReadValue(r, &status) },
		"Peek":         func() { r.Peek() },
		"NowNs":        func() { psmsgr.NowNs() },
	} {
		if n := testing.AllocsPerRun(100, f); n != 0 {
			t.Errorf("%s: %v allocations per call", name, n)
		}
	}
}

func benchPublish(b *testing.B, size int) {
	w, err := psmsgr.OpenWriter(chanName, uint32(size), &psmsgr.WriterOptions{Dir: b.TempDir()})
	if err != nil {
		b.Fatal(err)
	}
	defer w.Close()
	data := make([]byte, size)
	b.SetBytes(int64(size))
	for b.Loop() {
		w.Publish(data)
	}
}

func benchRead(b *testing.B, size int) {
	dir := b.TempDir()
	w, err := psmsgr.OpenWriter(chanName, uint32(size), &psmsgr.WriterOptions{Dir: dir})
	if err != nil {
		b.Fatal(err)
	}
	defer w.Close()
	w.Publish(make([]byte, size))
	r, err := psmsgr.OpenReader(chanName, dir)
	if err != nil {
		b.Fatal(err)
	}
	defer r.Close()
	buf := make([]byte, 0, size)
	b.SetBytes(int64(size))
	for b.Loop() {
		buf, _, _, _ = r.Read(buf[:0])
	}
}

func BenchmarkPublish16(b *testing.B) { benchPublish(b, 16) }
func BenchmarkPublish4K(b *testing.B) { benchPublish(b, 4096) }
func BenchmarkRead16(b *testing.B)    { benchRead(b, 16) }
func BenchmarkRead4K(b *testing.B)    { benchRead(b, 4096) }

func BenchmarkPeek(b *testing.B) {
	dir := b.TempDir()
	w, err := psmsgr.OpenWriter(chanName, 16, &psmsgr.WriterOptions{Dir: dir})
	if err != nil {
		b.Fatal(err)
	}
	defer w.Close()
	w.Publish(nil)
	r, err := psmsgr.OpenReader(chanName, dir)
	if err != nil {
		b.Fatal(err)
	}
	defer r.Close()
	for b.Loop() {
		r.Peek()
	}
}

func BenchmarkReadValue(b *testing.B) {
	dir := b.TempDir()
	w, err := psmsgr.OpenWriter(chanName, 24, &psmsgr.WriterOptions{Dir: dir})
	if err != nil {
		b.Fatal(err)
	}
	defer w.Close()
	status := motorStatus{Sequence: 1}
	psmsgr.PublishValue(w, &status)
	r, err := psmsgr.OpenReader(chanName, dir)
	if err != nil {
		b.Fatal(err)
	}
	defer r.Close()
	for b.Loop() {
		psmsgr.ReadValue(r, &status)
	}
}
