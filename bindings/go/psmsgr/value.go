// SPDX-License-Identifier: Apache-2.0

package psmsgr

// #cgo noescape psmsgr_state_read
// #cgo nocallback psmsgr_state_read
// #include <psmsgr/psmsgr.h>
import "C"

import (
	"reflect"
	"runtime"
	"sync"
	"unsafe"
)

// PublishValue publishes the unsafe.Sizeof(*v) raw bytes of *v and returns
// the generation. T must hold no pointers (no pointer, slice, string, map,
// chan, func or interface fields); declare it with fixed-width fields that
// match the readers' definition, e.g. the C struct. Otherwise it fails with
// ErrInval.
func PublishValue[T any](w *Writer, v *T) (uint32, error) {
	if err := checkPlain[T]("publish", w.name); err != nil {
		return 0, err
	}
	size := unsafe.Sizeof(*v)
	if uint64(size) > uint64(w.capacity) {
		if w.h == nil {
			return 0, ErrClosed
		}
		return 0, w.tooBig("publish", int(size))
	}
	return w.publish(unsafe.Pointer(v), C.uint32_t(size))
}

// ReadValue reads the latest value straight into *v, which must hold no
// pointers (see PublishValue). ok is false if there is no value yet. It
// fails with ErrMismatch if the value's length is not unsafe.Sizeof(*v);
// *v may be overwritten then.
func ReadValue[T any](r *Reader, v *T) (_ Info, ok bool, _ error) {
	if err := checkPlain[T]("read", r.name); err != nil {
		return Info{}, false, err
	}
	h := r.h
	if h == nil {
		return Info{}, false, ErrClosed
	}
	size := unsafe.Sizeof(*v)
	var ni C.psmsgr_state_info
	rc, errno := C.psmsgr_state_read(h, unsafe.Pointer(v), cLen(int(size)), &ni)
	runtime.KeepAlive(r)
	switch {
	case rc == C.PSMSGR_OK && uintptr(ni.length) == size:
		return info(&ni, r.takeAttached(&ni)), true, nil
	case rc == C.PSMSGR_E_NODATA:
		return Info{}, false, nil
	case rc == C.PSMSGR_OK || rc == C.PSMSGR_E_TOOSMALL:
		r.pending = r.pending || ni.flags&C.PSMSGR_INFO_ATTACHED != 0
		return Info{}, false, r.mismatch("read", uint32(ni.length), size, reflect.TypeFor[T]().String())
	}
	return Info{}, false, newError("read", r.name, rc, errno)
}

// plainTypes caches checkPlain's verdict per type.
var plainTypes sync.Map // reflect.Type -> bool

// checkPlain fails unless T is plain data: copying its bytes between
// processes, or writing arbitrary bytes into it, must not create pointers.
func checkPlain[T any](op, channel string) error {
	t := reflect.TypeFor[T]()
	ok, found := plainTypes.Load(t)
	if !found {
		ok, _ = plainTypes.LoadOrStore(t, isPlain(t))
	}
	if ok.(bool) {
		return nil
	}
	return &Error{Op: op, Channel: channel, Code: ErrInval, Msg: t.String() + " holds pointers"}
}

func isPlain(t reflect.Type) bool {
	switch t.Kind() {
	case reflect.Bool,
		reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64,
		reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr,
		reflect.Float32, reflect.Float64, reflect.Complex64, reflect.Complex128:
		return true
	case reflect.Array:
		return t.Len() == 0 || isPlain(t.Elem())
	case reflect.Struct:
		for i := range t.NumField() {
			if !isPlain(t.Field(i).Type) {
				return false
			}
		}
		return true
	}
	return false
}
