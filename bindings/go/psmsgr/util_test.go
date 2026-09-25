// SPDX-License-Identifier: Apache-2.0

package psmsgr_test

import (
	"bufio"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"

	"github.com/ps-solucoes/ps-msgr/bindings/go/psmsgr"
)

const chanName = "chan"

// channel is a test's own channel directory.
type channel struct {
	t   *testing.T
	dir string
}

func newChannel(t *testing.T) *channel {
	return &channel{t: t, dir: t.TempDir()}
}

func (c *channel) dataPath() string { return filepath.Join(c.dir, "psmsgr."+chanName+".state") }
func (c *channel) lockPath() string { return filepath.Join(c.dir, "psmsgr."+chanName+".lock") }

// open opens a writer of chanName in the test's directory, closed at the
// end of the test.
func (c *channel) open(capacity uint32, o psmsgr.WriterOptions) (*psmsgr.Writer, error) {
	o.Dir = c.dir
	w, err := psmsgr.OpenWriter(chanName, capacity, &o)
	if err == nil {
		c.t.Cleanup(func() { w.Close() })
	}
	return w, err
}

func (c *channel) writer(capacity uint32, o psmsgr.WriterOptions) *psmsgr.Writer {
	c.t.Helper()
	w, err := c.open(capacity, o)
	if err != nil {
		c.t.Fatal(err)
	}
	return w
}

func (c *channel) reader() *psmsgr.Reader {
	c.t.Helper()
	r, err := psmsgr.OpenReader(chanName, c.dir)
	if err != nil {
		c.t.Fatal(err)
	}
	c.t.Cleanup(func() { r.Close() })
	return r
}

// genAfter is the generation n publishes after gen: a new channel starts at
// a random one, and 0 is skipped on wrap.
func genAfter(gen uint32, n int) uint32 {
	for range n {
		gen++
		if gen == 0 {
			gen = 1
		}
	}
	return gen
}

func inode(t *testing.T, path string) uint64 {
	t.Helper()
	var st syscall.Stat_t
	if err := syscall.Stat(path, &st); err != nil {
		t.Fatal(err)
	}
	return st.Ino
}

func must[T any](t *testing.T) func(T, error) T {
	return func(v T, err error) T {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
		return v
	}
}

// ok unwraps a (value, ok, err) result that must have a value.
func ok[T any](t *testing.T) func(T, bool, error) T {
	return func(v T, ok bool, err error) T {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
		if !ok {
			t.Fatal("no value")
		}
		return v
	}
}

// none checks a (value, ok, err) result without a value.
func none[T any](t *testing.T) func(T, bool, error) {
	return func(v T, ok bool, err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
		if ok {
			t.Fatalf("unexpected value %+v", v)
		}
	}
}

// wantCode checks that err is an *psmsgr.Error with code.
func wantCode(t *testing.T, err error, code psmsgr.Code) *psmsgr.Error {
	t.Helper()
	e, isErr := err.(*psmsgr.Error)
	if !isErr {
		t.Fatalf("got %v (%T), want an *Error with code %d", err, err, code)
	}
	if e.Code != code {
		t.Fatalf("got code %d (%v), want %d", e.Code, e, code)
	}
	return e
}

// motorStatus is the C struct motor_status of the documentation. The
// explicit padding makes it 24 bytes on 32-bit ARM too, where Go aligns a
// uint64 to 4 bytes but C to 8.
type motorStatus struct {
	Sequence     uint64
	SpeedRpm     float32
	CurrentA     float32
	TemperatureC float32
	_            uint32
}

const motorStatusV1 = 0x0001_0001

// childWriter is a C writer in another process (tests/interop_helper,
// interop/README.md) that publishes a motorStatus per sequence and holds
// the channel until killed.
type childWriter struct {
	cmd   *exec.Cmd
	stdin io.Closer
}

func startChildWriter(t *testing.T, dir string, sequences ...uint64) *childWriter {
	t.Helper()
	build := os.Getenv("PSMSGR_BUILD_DIR")
	if build == "" {
		t.Skip("PSMSGR_BUILD_DIR is not set, so there is no build tree with tests/interop_helper")
	}
	args := []string{"write", dir, chanName, "--hold"}
	for _, s := range sequences {
		args = append(args, strconv.FormatUint(s, 10))
	}
	cmd := exec.Command(filepath.Join(build, "tests", "interop_helper"), args...)
	stdin := must[io.WriteCloser](t)(cmd.StdinPipe())
	stdout := must[io.ReadCloser](t)(cmd.StdoutPipe())
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	c := &childWriter{cmd: cmd, stdin: stdin}
	t.Cleanup(c.kill)
	lines := bufio.NewScanner(stdout)
	next := func() string {
		if !lines.Scan() {
			t.Fatal("interop_helper exited")
		}
		return lines.Text()
	}
	if l := next(); l != `{"opened":true}` {
		t.Fatalf("interop_helper: %s", l)
	}
	for range sequences {
		if l := next(); !strings.HasPrefix(l, `{"generation":`) {
			t.Fatalf("interop_helper: %s", l)
		}
	}
	return c
}

// kill sends SIGKILL: the writer lock goes with the process.
func (c *childWriter) kill() {
	if c.cmd.ProcessState == nil {
		c.cmd.Process.Kill()
		c.cmd.Wait()
	}
}
