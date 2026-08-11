package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"strconv"

	"github.com/higress-group/proxy-wasm-go-sdk/proxywasm"
	"github.com/higress-group/proxy-wasm-go-sdk/proxywasm/types"
)

func main() {}

func init() {
	proxywasm.SetPluginContext(func(contextID uint32) types.PluginContext {
		return &workerInitPluginContext{contextID: contextID}
	})
}

type pluginConfig struct {
	Mode        string `json:"mode"`
	RunID       string `json:"run_id"`
	WorkerCount int    `json:"worker_count"`
	Generation  string `json:"generation"`
}

type workerInitPluginContext struct {
	types.DefaultPluginContext
	contextID  uint32
	generation string
}

func (ctx *workerInitPluginContext) OnPluginStart(int) types.OnPluginStartStatus {
	configuration, err := proxywasm.GetPluginConfiguration()
	if err != nil {
		proxywasm.LogCriticalf("worker-init-repro: read configuration failed: %v", err)
		return types.OnPluginStartStatusFailed
	}

	var config pluginConfig
	if err := json.Unmarshal(configuration, &config); err != nil {
		proxywasm.LogCriticalf("worker-init-repro: decode configuration failed: %v", err)
		return types.OnPluginStartStatusFailed
	}
	if config.RunID == "" {
		proxywasm.LogCritical("worker-init-repro: run_id must not be empty")
		return types.OnPluginStartStatusFailed
	}
	if config.WorkerCount < 1 {
		proxywasm.LogCriticalf("worker-init-repro: worker_count must be positive, got %d",
			config.WorkerCount)
		return types.OnPluginStartStatusFailed
	}
	if config.Generation == "" {
		proxywasm.LogCritical("worker-init-repro: generation must not be empty")
		return types.OnPluginStartStatusFailed
	}
	ctx.generation = config.Generation

	attempt, err := nextAttempt("worker-init-repro/" + config.RunID)
	if err != nil {
		proxywasm.LogCriticalf("worker-init-repro: shared attempt update failed: %v", err)
		return types.OnPluginStartStatusFailed
	}
	proxywasm.LogWarnf("worker-init-repro: plugin start mode=%s attempt=%d context_id=%d generation=%s run_id=%s worker_count=%d",
		config.Mode, attempt, ctx.contextID, config.Generation, config.RunID, config.WorkerCount)

	// Attempt one is the createWasm canary. Main and worker TLS callbacks race after that, so the
	// injector deliberately covers every one of those callbacks without assigning roles by ordinal.
	if attempt == 1 {
		return types.OnPluginStartStatusOK
	}

	switch config.Mode {
	case "configure-reject":
		return types.OnPluginStartStatusFailed
	case "trap-once":
		if attempt <= 2+config.WorkerCount {
			panic("worker-init-repro: intentional one-shot worker initialization trap")
		}
	case "trap-always":
		panic("worker-init-repro: intentional persistent worker initialization trap")
	case "healthy":
		// No failure injection.
	default:
		proxywasm.LogCriticalf("worker-init-repro: unsupported mode %q", config.Mode)
		return types.OnPluginStartStatusFailed
	}
	return types.OnPluginStartStatusOK
}

func (ctx *workerInitPluginContext) NewHttpContext(uint32) types.HttpContext {
	return &workerInitHTTPContext{generation: ctx.generation}
}

type workerInitHTTPContext struct {
	types.DefaultHttpContext
	generation string
}

func (ctx *workerInitHTTPContext) OnHttpRequestHeaders(int, bool) types.Action {
	trap, _ := proxywasm.GetHttpRequestHeader("x-worker-init-request-trap")
	if trap == "true" {
		proxywasm.LogCriticalf("worker-init-repro: intentional request trap generation=%s",
			ctx.generation)
		panic("worker-init-repro: intentional request-stage trap")
	}
	_ = proxywasm.AddHttpRequestHeader("x-worker-init-plugin", "ready")
	return types.ActionContinue
}

func (ctx *workerInitHTTPContext) OnHttpResponseHeaders(int, bool) types.Action {
	_ = proxywasm.AddHttpResponseHeader("x-worker-init-generation", ctx.generation)
	return types.ActionContinue
}

func nextAttempt(key string) (int, error) {
	for retry := 0; retry < 32; retry++ {
		value, cas, err := proxywasm.GetSharedData(key)
		if errors.Is(err, types.ErrorStatusNotFound) {
			if err := proxywasm.SetSharedData(key, []byte("1"), 0); err != nil {
				return 0, err
			}
			return 1, nil
		}
		if err != nil {
			return 0, err
		}

		current, err := strconv.Atoi(string(value))
		if err != nil {
			return 0, fmt.Errorf("invalid shared attempt %q: %w", value, err)
		}
		next := current + 1
		err = proxywasm.SetSharedData(key, []byte(strconv.Itoa(next)), cas)
		if errors.Is(err, types.ErrorStatusCasMismatch) {
			continue
		}
		if err != nil {
			return 0, err
		}
		return next, nil
	}
	return 0, fmt.Errorf("shared attempt CAS did not converge")
}
