// SPDX-License-Identifier: Apache-2.0

//go:build !psmsgr_static

package psmsgr

// #cgo LDFLAGS: -lpsmsgr
import "C"
