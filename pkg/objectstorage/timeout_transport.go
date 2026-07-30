package objectstorage

import (
	"context"
	"io"
	"net/http"
	"time"
)

type timeoutRoundTripper struct {
	base    http.RoundTripper
	timeout time.Duration
}

func (t timeoutRoundTripper) RoundTrip(req *http.Request) (*http.Response, error) {
	ctx := req.Context()
	deadline, ok := ctx.Deadline()
	if !ok || time.Until(deadline) > t.timeout {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, t.timeout)
		req = req.WithContext(ctx)
		resp, err := t.base.RoundTrip(req)
		if err != nil {
			cancel()
			return nil, err
		}
		resp.Body = cancelReadCloser{
			ReadCloser: resp.Body,
			cancel:     cancel,
		}
		return resp, nil
	}
	return t.base.RoundTrip(req)
}

type cancelReadCloser struct {
	io.ReadCloser
	cancel context.CancelFunc
}

func (c cancelReadCloser) Close() error {
	err := c.ReadCloser.Close()
	c.cancel()
	return err
}
