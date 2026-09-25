// SPDX-License-Identifier: Apache-2.0

package psmsgr

import (
	"errors"
	"strconv"
	"strings"
	"syscall"
)

// Code is a libpsmsgr result code (PSMSGR_E_*). It is an error itself, so
// that errors.Is(err, ErrBusy) matches an [*Error] with that code.
type Code int32

// The result codes. The comments say where the package returns them.
const (
	ErrInval        Code = -1  // bad argument or channel name
	ErrSys          Code = -2  // system call failed: see Error.Errno
	ErrNoData       Code = -3  // never returned: ok == false instead
	ErrTooSmall     Code = -4  // never returned: Read grows its buffer
	ErrTooBig       Code = -5  // payload larger than the capacity
	ErrBusy         Code = -6  // read retries exhausted: transient, retry
	ErrTimeout      Code = -7  // never returned: Wait returns false
	ErrIntr         Code = -8  // never returned: Wait retries
	ErrWriterExists Code = -9  // another writer holds the channel
	ErrMismatch     Code = -10 // other geometry, or ReadValue's type has another size
	ErrFormat       Code = -11 // not a channel file, or a corrupt one
	ErrNotSup       Code = -12 // e.g. Wait on a channel without notification
	ErrState        Code = -13 // e.g. Publish from inside PublishFunc
)

// Error is the library's text for the code (psmsgr_strerror).
func (c Code) Error() string { return strerror(c) }

// Error is a failed call.
type Error struct {
	Op      string        // the call, e.g. "open writer", "publish"
	Channel string        // the channel name
	Code    Code          // the result code
	Errno   syscall.Errno // the system error for ErrSys, else 0
	Msg     string        // the binding's own detail; empty: the code's text
}

// Error formats as `psmsgr: <op> "<channel>": <text>`, where text is Msg,
// else the errno's text for ErrSys, else the code's.
func (e *Error) Error() string {
	var b strings.Builder
	b.WriteString("psmsgr: ")
	b.WriteString(e.Op)
	if e.Channel != "" {
		b.WriteByte(' ')
		b.WriteString(strconv.Quote(e.Channel))
	}
	b.WriteString(": ")
	switch {
	case e.Msg != "":
		b.WriteString(e.Msg)
	case e.Code == ErrSys:
		b.WriteString(e.Errno.Error())
	default:
		b.WriteString(e.Code.Error())
	}
	return b.String()
}

// Is reports whether target is the error's Code.
func (e *Error) Is(target error) bool {
	c, ok := target.(Code)
	return ok && c == e.Code
}

// Unwrap returns the Errno, if any, so that errors.Is(err, fs.ErrNotExist)
// and errors.Is(err, syscall.ENOENT) work.
func (e *Error) Unwrap() error {
	if e.Errno == 0 {
		return nil
	}
	return e.Errno
}

// ErrClosed is returned by calls on a closed [Writer] or [Reader].
var ErrClosed = errors.New("psmsgr: use of closed handle")
