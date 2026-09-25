// SPDX-License-Identifier: Apache-2.0

//go:build psmsgr_static

package psmsgr

// libpsmsgr.a needs nothing but libc, so the executable needs no libpsmsgr.

// #cgo LDFLAGS: -l:libpsmsgr.a
import "C"
