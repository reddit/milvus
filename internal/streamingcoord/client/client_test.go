package client

import (
	"net"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"

	kvfactory "github.com/milvus-io/milvus/internal/util/dependency/kv"
	"github.com/milvus-io/milvus/pkg/v2/util/paramtable"
)

func TestDial(t *testing.T) {
	conn, err := net.DialTimeout("tcp", "localhost:2379", 200*time.Millisecond)
	if err != nil {
		t.Skip("etcd is not available")
	}
	_ = conn.Close()

	paramtable.Init()

	c, _ := kvfactory.GetEtcdAndPath()
	assert.NotNil(t, c)

	client := NewClient(c)
	assert.NotNil(t, client)
	client.Close()
}
