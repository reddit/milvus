// Licensed to the LF AI & Data foundation under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership. The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package metrics

import (
	"testing"

	"github.com/prometheus/client_golang/prometheus"
	clientmodel "github.com/prometheus/client_model/go"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
	"golang.org/x/sync/errgroup"
)

func TestRegisterMetrics(t *testing.T) {
	assert.NotPanics(t, func() {
		r := prometheus.NewRegistry()
		// Make sure it doesn't panic.
		RegisterMixCoord(r)
		RegisterDataNode(r)
		RegisterProxy(r)
		RegisterQueryNode(r)
		RegisterMetaMetrics(r)
		RegisterStorageMetrics(r)
		RegisterMsgStreamMetrics(r)
		RegisterCGOMetrics(r)
		RegisterStreamingServiceClient(r)
		RegisterStreamingNode(r)
		RegisterLoggingMetrics(r)
	})
}

func TestProxySQLatencySLO(t *testing.T) {
	registry := prometheus.NewRegistry()
	registry.MustRegister(ProxySQLatencySLO)
	registry.MustRegister(ProxySQLatencyGranular)

	const (
		nodeID         = "classic-histogram-test"
		queryType      = "search"
		databaseName   = "classic-histogram-test"
		collectionName = "classic-histogram-test"
	)
	ProxySQLatencySLO.WithLabelValues(nodeID, queryType, databaseName, collectionName).Observe(501)
	ProxySQLatencyGranular.WithLabelValues(nodeID, queryType, databaseName, collectionName).Observe(501)

	metricFamilies, err := registry.Gather()
	require.NoError(t, err)

	var sloHistogram, granularHistogram *clientmodel.Histogram
	for _, family := range metricFamilies {
		for _, metric := range family.GetMetric() {
			if metricHasLabel(metric, nodeIDLabelName, nodeID) {
				switch family.GetName() {
				case "milvus_proxy_sq_latency_slo":
					sloHistogram = metric.GetHistogram()
				case "milvus_proxy_sq_latency_granular":
					granularHistogram = metric.GetHistogram()
				}
				break
			}
		}
	}
	require.NotNil(t, sloHistogram)
	require.NotNil(t, granularHistogram)

	upperBounds := make([]float64, 0, len(sloHistogram.GetBucket()))
	for _, bucket := range sloHistogram.GetBucket() {
		upperBounds = append(upperBounds, bucket.GetUpperBound())
	}
	assert.Equal(t, sqLatencySLOBuckets, upperBounds)
	assert.Nil(t, sloHistogram.Schema, "SLO histogram must be classic-only")
	assert.Empty(t, granularHistogram.GetBucket(), "granular histogram must remain native-only")
	assert.NotNil(t, granularHistogram.Schema, "granular native histogram must remain enabled")
}

func metricHasLabel(metric *clientmodel.Metric, name, value string) bool {
	for _, label := range metric.GetLabel() {
		if label.GetName() == name && label.GetValue() == value {
			return true
		}
	}
	return false
}

func TestGetRegisterer(t *testing.T) {
	register := GetRegisterer()
	assert.NotNil(t, register)
	assert.Equal(t, prometheus.DefaultRegisterer, register)
	r := prometheus.NewRegistry()
	Register(r)
	register = GetRegisterer()
	assert.NotNil(t, register)
	assert.Equal(t, r, register)
}

func TestRegisterRuntimeInfo(t *testing.T) {
	g := &errgroup.Group{}
	g.Go(func() error {
		RegisterMetaType("etcd")
		return nil
	})
	g.Go(func() error {
		RegisterMQType("pulsar")
		return nil
	})
	g.Wait()

	infoMutex.Lock()
	defer infoMutex.Unlock()
	assert.Equal(t, "etcd", metaType)
	assert.Equal(t, "pulsar", mqType)
}

// TestCleanupQueryNodeCollectionMetrics tests that CleanupQueryNodeCollectionMetrics
// correctly cleans up all metrics for a given nodeID and collectionID.
func TestCleanupQueryNodeCollectionMetrics(t *testing.T) {
	nodeID := int64(1)
	collectionID := int64(100)
	nodeIDStr := "1"
	collectionIDStr := "100"

	// Set up some metrics for the collection
	// QueryNodeConsumerMsgCount: nodeID, msgType, collectionID
	QueryNodeConsumerMsgCount.WithLabelValues(nodeIDStr, "insert", collectionIDStr).Add(10)
	// QueryNodeConsumeTimeTickLag: nodeID, msgType, collectionID
	QueryNodeConsumeTimeTickLag.WithLabelValues(nodeIDStr, "insert", collectionIDStr).Set(5)
	// QueryNodeNumEntities: database, collectionName, nodeID, collectionID, segmentState
	QueryNodeNumEntities.WithLabelValues("default", "test_collection", nodeIDStr, collectionIDStr, "growing").Set(100)
	// QueryNodeEntitiesSize: nodeID, collectionID, segmentState
	QueryNodeEntitiesSize.WithLabelValues(nodeIDStr, collectionIDStr, "growing").Set(1024)
	// QueryNodeNumSegments: nodeID, collectionID, segmentState, segmentLevel
	QueryNodeNumSegments.WithLabelValues(nodeIDStr, collectionIDStr, "sealed", "L1").Set(5)
	// QueryNodeSQCount: nodeID, queryType, status, requestScope, collectionID
	QueryNodeSQCount.WithLabelValues(nodeIDStr, "search", "success", "default", collectionIDStr).Add(50)
	// QueryNodeLevelZeroSize: nodeID, collectionID, channelName
	QueryNodeLevelZeroSize.WithLabelValues(nodeIDStr, collectionIDStr, "ch1").Set(256)

	// Set up metrics for a different collection (should not be cleaned up)
	otherCollectionIDStr := "200"
	QueryNodeConsumerMsgCount.WithLabelValues(nodeIDStr, "insert", otherCollectionIDStr).Add(20)
	QueryNodeNumEntities.WithLabelValues("default", "other_collection", nodeIDStr, otherCollectionIDStr, "growing").Set(200)

	// Helper function to count metrics
	countCounterMetrics := func(vec *prometheus.CounterVec) int {
		ch := make(chan prometheus.Metric, 100)
		vec.Collect(ch)
		close(ch)
		count := 0
		for range ch {
			count++
		}
		return count
	}

	countGaugeMetrics := func(vec *prometheus.GaugeVec) int {
		ch := make(chan prometheus.Metric, 100)
		vec.Collect(ch)
		close(ch)
		count := 0
		for range ch {
			count++
		}
		return count
	}

	// Record counts before cleanup
	consumerCountBefore := countCounterMetrics(QueryNodeConsumerMsgCount)
	numEntitiesBefore := countGaugeMetrics(QueryNodeNumEntities)

	// Clean up metrics for the target collection
	CleanupQueryNodeCollectionMetrics(nodeID, collectionID)

	// Verify that the target collection's metrics are cleaned up
	// and other collection's metrics still exist
	consumerCountAfter := countCounterMetrics(QueryNodeConsumerMsgCount)
	numEntitiesAfter := countGaugeMetrics(QueryNodeNumEntities)

	// At least one metric should be removed from each
	assert.Less(t, consumerCountAfter, consumerCountBefore)
	assert.Less(t, numEntitiesAfter, numEntitiesBefore)

	// Other collection's metrics should still exist
	assert.Greater(t, consumerCountAfter, 0)
	assert.Greater(t, numEntitiesAfter, 0)
}

// TestDeletePartialMatch test deletes all metrics where the variable labels contain all of those
// passed in as labels based on DeletePartialMatch API
func TestDeletePartialMatch(t *testing.T) {
	baseVec := prometheus.NewGaugeVec(
		prometheus.GaugeOpts{
			Name: "test",
			Help: "helpless",
		},
		[]string{"l1", "l2", "l3"},
	)

	baseVec.WithLabelValues("l1-1", "l2-1", "l3-1").Inc()
	baseVec.WithLabelValues("l1-2", "l2-2", "l3-2").Inc()
	baseVec.WithLabelValues("l1-2", "l2-3", "l3-3").Inc()

	baseVec.WithLabelValues("l1-3", "l2-3", "l3-3").Inc()
	baseVec.WithLabelValues("l1-3", "l2-3", "").Inc()
	baseVec.WithLabelValues("l1-3", "l2-4", "l3-4").Inc()

	baseVec.WithLabelValues("l1-4", "l2-5", "l3-5").Inc()
	baseVec.WithLabelValues("l1-4", "l2-5", "l3-6").Inc()
	baseVec.WithLabelValues("l1-5", "l2-6", "l3-6").Inc()

	getMetricsCount := func() int {
		chs := make(chan prometheus.Metric, 10)
		baseVec.Collect(chs)
		return len(chs)
	}

	// the prefix is matched which has one labels
	if got, want := baseVec.DeletePartialMatch(prometheus.Labels{"l1": "l1-2"}), 2; got != want {
		t.Errorf("got %v, want %v", got, want)
	}
	assert.Equal(t, 7, getMetricsCount())

	// the prefix is matched which has two labels
	if got, want := baseVec.DeletePartialMatch(prometheus.Labels{"l1": "l1-3", "l2": "l2-3"}), 2; got != want {
		t.Errorf("got %v, want %v", got, want)
	}
	assert.Equal(t, 5, getMetricsCount())

	// the first and latest labels are matched
	if got, want := baseVec.DeletePartialMatch(prometheus.Labels{"l1": "l1-1", "l3": "l3-1"}), 1; got != want {
		t.Errorf("got %v, want %v", got, want)
	}
	assert.Equal(t, 4, getMetricsCount())

	// the middle labels are matched
	if got, want := baseVec.DeletePartialMatch(prometheus.Labels{"l2": "l2-5"}), 2; got != want {
		t.Errorf("got %v, want %v", got, want)
	}
	assert.Equal(t, 2, getMetricsCount())

	// the middle labels and suffix labels are matched
	if got, want := baseVec.DeletePartialMatch(prometheus.Labels{"l2": "l2-6", "l3": "l3-6"}), 1; got != want {
		t.Errorf("got %v, want %v", got, want)
	}
	assert.Equal(t, 1, getMetricsCount())

	// all labels are matched
	if got, want := baseVec.DeletePartialMatch(prometheus.Labels{"l1": "l1-3", "l2": "l2-4", "l3": "l3-4"}), 1; got != want {
		t.Errorf("got %v, want %v", got, want)
	}
	assert.Equal(t, 0, getMetricsCount())
}
