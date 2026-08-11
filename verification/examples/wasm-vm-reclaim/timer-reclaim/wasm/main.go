package main

import (
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"strconv"

	"github.com/higress-group/proxy-wasm-go-sdk/proxywasm"
	"github.com/higress-group/proxy-wasm-go-sdk/proxywasm/types"
)

// allocBuffer is retained on the VM (per generation) so linear memory stays
// high until the generation is reclaimed. A reclaimed (newly cloned) VM starts
// with an empty buffer, which is exactly the behavior the memory-threshold
// reclaim experiment observes.
var allocBuffer []byte

func main() {}

func init() {
	proxywasm.SetPluginContext(func(contextID uint32) types.PluginContext {
		return &reclaimPluginContext{contextID: contextID}
	})
}

type reclaimPluginContext struct {
	types.DefaultPluginContext
	contextID   uint32
	workerToken string
}

func (ctx *reclaimPluginContext) OnPluginStart(int) types.OnPluginStartStatus {
	token := make([]byte, 8)
	if _, err := rand.Read(token); err != nil {
		proxywasm.LogCriticalf("reclaim-verify: worker token generation failed: %v", err)
		return types.OnPluginStartStatusFailed
	}
	ctx.workerToken = hex.EncodeToString(token)
	proxywasm.LogDebugf("reclaim-verify: plugin started context_id=%d worker_token=%s",
		ctx.contextID, ctx.workerToken)
	return types.OnPluginStartStatusOK
}

func (ctx *reclaimPluginContext) NewHttpContext(contextID uint32) types.HttpContext {
	return &reclaimContext{contextID: contextID, workerToken: ctx.workerToken}
}

type reclaimContext struct {
	types.DefaultHttpContext
	contextID    uint32
	workerToken  string
	requestID    string
	retainedSize int
	vmMemory     uint64
}

func (ctx *reclaimContext) OnHttpRequestHeaders(numHeaders int, endOfStream bool) types.Action {
	ctx.requestID, _ = proxywasm.GetHttpRequestHeader("x-reclaim-request-id")
	if ctx.requestID == "" {
		ctx.requestID = "unlabeled"
	}

	// Trigger source 1 (explicit flag): set host-visible shouldRebuild(true).
	if v, _ := proxywasm.GetHttpRequestHeader("x-set-rebuild"); v == "true" {
		_ = proxywasm.SetProperty([]string{"wasm_need_rebuild"}, []byte("true"))
		proxywasm.LogDebugf("reclaim-verify: set wasm_need_rebuild, context_id=%d", ctx.contextID)
	}

	// Trigger source 2 (memory threshold): ensure the VM retains at least N MiB
	// and touch every new page so repeated worker-steering requests are idempotent.
	if v, _ := proxywasm.GetHttpRequestHeader("x-alloc-mb"); v != "" {
		if mb, err := strconv.Atoi(v); err == nil && mb > 0 {
			target := mb * 1024 * 1024
			if target > len(allocBuffer) {
				previousSize := len(allocBuffer)
				grown := make([]byte, target)
				copy(grown, allocBuffer)
				for i := previousSize; i < len(grown); i += 4096 {
					grown[i] = 1
				}
				allocBuffer = grown
			}
			proxywasm.LogDebugf("reclaim-verify: ensured %d MiB, retained total %d MiB, context_id=%d",
				mb, len(allocBuffer)/(1024*1024), ctx.contextID)
		}
	}

	ctx.retainedSize = len(allocBuffer)
	if data, err := proxywasm.GetProperty([]string{"plugin_vm_memory"}); err == nil && len(data) == 8 {
		ctx.vmMemory = binary.LittleEndian.Uint64(data)
		proxywasm.LogDebugf("reclaim-verify: VM memory %d bytes (%.2f MiB), context_id=%d request_id=%s",
			ctx.vmMemory, float64(ctx.vmMemory)/(1024*1024), ctx.contextID, ctx.requestID)
	}

	// Holding a stream and making its generation eligible are separate controls.
	// The harness combines them for the first A -> B cutover and holds B without
	// implicitly changing the trigger source when checking the live-old guard.
	if v, _ := proxywasm.GetHttpRequestHeader("x-hold-active"); v == "true" {
		delay := "6"
		if requestedDelay, _ := proxywasm.GetHttpRequestHeader("x-hold-delay"); requestedDelay != "" {
			delay = requestedDelay
		}
		proxywasm.LogDebugf("reclaim-verify: holding active request for %ss, context_id=%d request_id=%s retained_bytes=%d worker_token=%s",
			delay, ctx.contextID, ctx.requestID, ctx.retainedSize, ctx.workerToken)
		_, err := proxywasm.DispatchHttpCall(
			"outbound|80||httpbin.dns",
			[][2]string{
				{":method", "GET"},
				{":path", "/slow?delay=" + delay},
				{":authority", "httpbin.dns"},
			},
			nil,
			nil,
			30000,
			func(numHeaders, bodySize, numTrailers int) {
				proxywasm.LogDebugf("reclaim-verify: hold callback, context_id=%d request_id=%s retained_bytes=%d worker_token=%s",
					ctx.contextID, ctx.requestID, ctx.retainedSize, ctx.workerToken)
				if err := proxywasm.ResumeHttpRequest(); err != nil {
					proxywasm.LogErrorf("reclaim-verify: resume failed: %v", err)
				}
			})
		if err != nil {
			proxywasm.LogErrorf("reclaim-verify: dispatch hold call failed: %v", err)
			return types.ActionContinue
		}
		return types.ActionPause
	}

	_ = proxywasm.AddHttpRequestHeader("x-reclaim-verify-plugin", "ok")
	proxywasm.LogDebugf("reclaim-verify: request passed context_id=%d request_id=%s retained_bytes=%d",
		ctx.contextID, ctx.requestID, ctx.retainedSize)
	return types.ActionContinue
}

func (ctx *reclaimContext) OnHttpResponseHeaders(numHeaders int, endOfStream bool) types.Action {
	_ = proxywasm.AddHttpResponseHeader("x-reclaim-worker-token", ctx.workerToken)
	_ = proxywasm.AddHttpResponseHeader("x-reclaim-request-id", ctx.requestID)
	_ = proxywasm.AddHttpResponseHeader("x-reclaim-retained-bytes", strconv.Itoa(ctx.retainedSize))
	_ = proxywasm.AddHttpResponseHeader("x-reclaim-vm-memory-bytes", strconv.FormatUint(ctx.vmMemory, 10))
	return types.ActionContinue
}
