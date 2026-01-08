package planparserv2

import (
	"fmt"
	"testing"

	"github.com/stretchr/testify/assert"

	"github.com/milvus-io/milvus-proto/go-api/v2/commonpb"
	"github.com/milvus-io/milvus-proto/go-api/v2/schemapb"
	"github.com/milvus-io/milvus/pkg/v2/proto/planpb"
	"github.com/milvus-io/milvus/pkg/v2/util/typeutil"
)

// newTestSchemaWithTextSearch creates a test schema with text-searchable fields
func newTestSchemaWithTextSearch() *schemapb.CollectionSchema {
	fields := []*schemapb.FieldSchema{
		{FieldID: 0, Name: "FieldID", IsPrimaryKey: false, Description: "field no.1", DataType: schemapb.DataType_Int64},
		{FieldID: 100, Name: "Int64Field", IsPrimaryKey: true, Description: "int64 field", DataType: schemapb.DataType_Int64},
		{FieldID: 101, Name: "FloatVectorField", IsPrimaryKey: false, Description: "float vector field", DataType: schemapb.DataType_FloatVector,
			TypeParams: []*commonpb.KeyValuePair{{Key: "dim", Value: "128"}}},
		{FieldID: 102, Name: "TextField1", IsPrimaryKey: false, Description: "text field 1", DataType: schemapb.DataType_VarChar,
			TypeParams: []*commonpb.KeyValuePair{
				{Key: "enable_match", Value: "True"},
				{Key: "max_length", Value: "1000"},
			}},
		{FieldID: 103, Name: "TextField2", IsPrimaryKey: false, Description: "text field 2", DataType: schemapb.DataType_VarChar,
			TypeParams: []*commonpb.KeyValuePair{
				{Key: "enable_match", Value: "True"},
				{Key: "max_length", Value: "1000"},
			}},
		{FieldID: 104, Name: "NormalField", IsPrimaryKey: false, Description: "normal varchar field", DataType: schemapb.DataType_VarChar,
			TypeParams: []*commonpb.KeyValuePair{
				{Key: "max_length", Value: "500"},
			}},
	}

	return &schemapb.CollectionSchema{
		Name:               "test_text_search",
		Description:        "schema for text_search test",
		AutoID:             true,
		Fields:             fields,
		EnableDynamicField: true,
	}
}

func newTestSchemaHelperWithTextSearch(t *testing.T) *typeutil.SchemaHelper {
	schema := newTestSchemaWithTextSearch()
	schemaHelper, err := typeutil.CreateSchemaHelper(schema)
	assert.NoError(t, err)
	return schemaHelper
}

// TestExpr_TextSearch_BasicSyntax tests basic text_search expression syntax
func TestExpr_TextSearch_BasicSyntax(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	// Valid expressions
	validExprs := []string{
		`text_search(TextField1, "query")`,
		`text_search(TextField2, "search terms")`,
		`text_search(TextField1, "multi word query")`,
	}
	for _, exprStr := range validExprs {
		expr, err := ParseExpr(schema, exprStr, nil)
		assert.NoError(t, err, "Expression should be valid: %s", exprStr)
		assert.NotNil(t, expr, "Expression should not be nil: %s", exprStr)
	}
}

// TestExpr_TextSearch_WithTopK tests text_search with topk option
func TestExpr_TextSearch_WithTopK(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	testCases := []struct {
		expr     string
		topk     int64
		hasError bool
	}{
		{`text_search(TextField1, "query", topk=10)`, 10, false},
		{`text_search(TextField1, "query", topk=100)`, 100, false},
		{`text_search(TextField1, "query", topk=1)`, 1, false},
		{`text_search(TextField1, "query", topk=0)`, 0, true},  // Invalid: zero
		{`text_search(TextField1, "query", topk=-1)`, 0, true}, // Invalid: negative
	}

	for _, tc := range testCases {
		expr, err := ParseExpr(schema, tc.expr, nil)
		if tc.hasError {
			assert.Error(t, err, "Expression should have error: %s", tc.expr)
		} else {
			assert.NoError(t, err, "Expression should be valid: %s", tc.expr)
			assert.NotNil(t, expr, "Expression should not be nil: %s", tc.expr)
			textSearchExpr := expr.GetTextSearchExpr()
			assert.NotNil(t, textSearchExpr, "Should be a text_search expression: %s", tc.expr)
			assert.Equal(t, tc.topk, textSearchExpr.GetTopk(), "TopK mismatch for: %s", tc.expr)
		}
	}
}

// TestExpr_TextSearch_MultiField tests text_search with multiple fields
func TestExpr_TextSearch_MultiField(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	// Multi-field text search
	expr, err := ParseExpr(schema, `text_search([TextField1, TextField2], "query")`, nil)
	assert.NoError(t, err)
	assert.NotNil(t, expr)
	textSearchExpr := expr.GetTextSearchExpr()
	assert.NotNil(t, textSearchExpr)
	assert.Len(t, textSearchExpr.GetFieldIds(), 2)
}

// TestExpr_TextSearch_WithWeights tests text_search with weights option
func TestExpr_TextSearch_WithWeights(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	// Single field with weight
	expr1, err := ParseExpr(schema, `text_search(TextField1, "query", weights=[1.0])`, nil)
	assert.NoError(t, err)
	assert.NotNil(t, expr1)
	textSearch1 := expr1.GetTextSearchExpr()
	assert.NotNil(t, textSearch1)
	assert.Len(t, textSearch1.GetWeights(), 1)
	assert.InDelta(t, 1.0, textSearch1.GetWeights()[0], 0.01)

	// Multi-field with weights
	expr2, err := ParseExpr(schema, `text_search([TextField1, TextField2], "query", weights=[0.5, 0.5])`, nil)
	assert.NoError(t, err)
	assert.NotNil(t, expr2)
	textSearch2 := expr2.GetTextSearchExpr()
	assert.NotNil(t, textSearch2)
	assert.Len(t, textSearch2.GetWeights(), 2)

	// Invalid: weights count mismatch
	_, err = ParseExpr(schema, `text_search([TextField1, TextField2], "query", weights=[0.5])`, nil)
	assert.Error(t, err, "Should error when weights count doesn't match field count")
}

// TestExpr_TextSearch_WithAggregation tests text_search with aggregation option
func TestExpr_TextSearch_WithAggregation(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	testCases := []struct {
		expr        string
		aggregation planpb.BM25AggregationType
		hasError    bool
	}{
		{`text_search([TextField1, TextField2], "query", aggregation="weighted_sum")`, planpb.BM25AggregationType_BM25AggWeightedSum, false},
		{`text_search([TextField1, TextField2], "query", aggregation="max")`, planpb.BM25AggregationType_BM25AggMax, false},
		{`text_search([TextField1, TextField2], "query", aggregation="invalid")`, 0, true},
	}

	for _, tc := range testCases {
		expr, err := ParseExpr(schema, tc.expr, nil)
		if tc.hasError {
			assert.Error(t, err, "Expression should have error: %s", tc.expr)
		} else {
			assert.NoError(t, err, "Expression should be valid: %s", tc.expr)
			assert.NotNil(t, expr, "Expression should not be nil: %s", tc.expr)
			textSearchExpr := expr.GetTextSearchExpr()
			assert.NotNil(t, textSearchExpr, "Should be a text_search expression: %s", tc.expr)
			assert.Equal(t, tc.aggregation, textSearchExpr.GetAggregation(), "Aggregation mismatch for: %s", tc.expr)
		}
	}
}

// TestExpr_TextSearch_WithSlop tests text_search with slop option
func TestExpr_TextSearch_WithSlop(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	testCases := []struct {
		expr     string
		slop     uint32
		hasError bool
	}{
		{`text_search(TextField1, "query", slop=0)`, 0, false},
		{`text_search(TextField1, "query", slop=1)`, 1, false},
		{`text_search(TextField1, "query", slop=10)`, 10, false},
	}

	for _, tc := range testCases {
		expr, err := ParseExpr(schema, tc.expr, nil)
		if tc.hasError {
			assert.Error(t, err, "Expression should have error: %s", tc.expr)
		} else {
			assert.NoError(t, err, "Expression should be valid: %s", tc.expr)
			assert.NotNil(t, expr, "Expression should not be nil: %s", tc.expr)
			textSearchExpr := expr.GetTextSearchExpr()
			assert.NotNil(t, textSearchExpr, "Should be a text_search expression: %s", tc.expr)
			assert.Equal(t, tc.slop, textSearchExpr.GetSlop(), "Slop mismatch for: %s", tc.expr)
		}
	}
}

// TestExpr_TextSearch_WithMinShouldMatch tests text_search with minimum_should_match option
func TestExpr_TextSearch_WithMinShouldMatch(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	testCases := []struct {
		expr           string
		minShouldMatch uint32
		hasError       bool
	}{
		{`text_search(TextField1, "query", minimum_should_match=1)`, 1, false},
		{`text_search(TextField1, "query", minimum_should_match=2)`, 2, false},
		{`text_search(TextField1, "query", minimum_should_match=10)`, 10, false},
	}

	for _, tc := range testCases {
		expr, err := ParseExpr(schema, tc.expr, nil)
		if tc.hasError {
			assert.Error(t, err, "Expression should have error: %s", tc.expr)
		} else {
			assert.NoError(t, err, "Expression should be valid: %s", tc.expr)
			assert.NotNil(t, expr, "Expression should not be nil: %s", tc.expr)
			textSearchExpr := expr.GetTextSearchExpr()
			assert.NotNil(t, textSearchExpr, "Should be a text_search expression: %s", tc.expr)
			assert.Equal(t, tc.minShouldMatch, textSearchExpr.GetMinimumShouldMatch(), "MinShouldMatch mismatch for: %s", tc.expr)
		}
	}
}

// TestExpr_TextSearch_CombinedOptions tests text_search with multiple options
func TestExpr_TextSearch_CombinedOptions(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	expr, err := ParseExpr(schema, `text_search([TextField1, TextField2], "search query", topk=50, weights=[0.7, 0.3], aggregation="max", slop=2, minimum_should_match=1)`, nil)
	assert.NoError(t, err)
	assert.NotNil(t, expr)

	textSearch := expr.GetTextSearchExpr()
	assert.NotNil(t, textSearch)
	assert.Equal(t, "search query", textSearch.GetQuery())
	assert.Equal(t, int64(50), textSearch.GetTopk())
	assert.Len(t, textSearch.GetFieldIds(), 2)
	assert.Len(t, textSearch.GetWeights(), 2)
	assert.InDelta(t, 0.7, textSearch.GetWeights()[0], 0.01)
	assert.InDelta(t, 0.3, textSearch.GetWeights()[1], 0.01)
	assert.Equal(t, planpb.BM25AggregationType_BM25AggMax, textSearch.GetAggregation())
	assert.Equal(t, uint32(2), textSearch.GetSlop())
	assert.Equal(t, uint32(1), textSearch.GetMinimumShouldMatch())
}

// TestExpr_TextSearch_InvalidField tests text_search with invalid fields
func TestExpr_TextSearch_InvalidField(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	invalidExprs := []string{
		// Non-existent field
		`text_search(NonExistentField, "query")`,
		// Non-string field
		`text_search(Int64Field, "query")`,
		// Field without enable_match
		`text_search(NormalField, "query")`,
	}

	for _, exprStr := range invalidExprs {
		_, err := ParseExpr(schema, exprStr, nil)
		assert.Error(t, err, "Expression should have error for invalid field: %s", exprStr)
	}
}

// TestExpr_TextSearch_DefaultValues tests text_search default option values
func TestExpr_TextSearch_DefaultValues(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	expr, err := ParseExpr(schema, `text_search(TextField1, "query")`, nil)
	assert.NoError(t, err)
	assert.NotNil(t, expr)

	textSearch := expr.GetTextSearchExpr()
	assert.NotNil(t, textSearch)
	assert.Equal(t, int64(10), textSearch.GetTopk())                                          // default topk is 10
	assert.Equal(t, planpb.BM25AggregationType_BM25AggWeightedSum, textSearch.GetAggregation()) // default aggregation
	assert.Equal(t, uint32(0), textSearch.GetSlop())                                           // default slop
	assert.Equal(t, uint32(0), textSearch.GetMinimumShouldMatch())                             // default min_should_match
}

// TestExpr_TextSearch_InSearchPlan tests using text_search in a search plan
func TestExpr_TextSearch_InSearchPlan(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	// Note: text_search is typically used as a predicate/filter in search plans
	// The actual BM25 scoring is handled via TEXT_BM25 metric type
	expr := `text_search(TextField1, "search query", topk=100)`

	plan, err := CreateSearchPlan(schema, expr, "FloatVectorField", &planpb.QueryInfo{
		Topk:         10,
		MetricType:   "L2",
		SearchParams: "",
		RoundDecimal: 0,
	}, nil, nil)

	assert.NoError(t, err, "Should be able to create search plan with text_search predicate")
	assert.NotNil(t, plan)
	assert.NotNil(t, plan.GetVectorAnns())
}

// TestExpr_TextSearch_QueryText tests various query text formats
func TestExpr_TextSearch_QueryText(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	queryTexts := []string{
		"simple",
		"multi word query",
		"special-characters_here",
		"numbers 123 456",
		"中文查询",
		"mixed English 和 中文",
	}

	for _, queryText := range queryTexts {
		exprStr := fmt.Sprintf(`text_search(TextField1, "%s")`, queryText)
		expr, err := ParseExpr(schema, exprStr, nil)
		assert.NoError(t, err, "Should handle query text: %s", queryText)
		assert.NotNil(t, expr)
		textSearch := expr.GetTextSearchExpr()
		assert.NotNil(t, textSearch)
		assert.Equal(t, queryText, textSearch.GetQuery())
	}
}

// TestExpr_TextSearch_CaseSensitivity tests case handling in text_search
func TestExpr_TextSearch_CaseSensitivity(t *testing.T) {
	schema := newTestSchemaHelperWithTextSearch(t)

	// Function name should be case-insensitive in ANTLR grammar
	exprs := []string{
		`text_search(TextField1, "query")`,
		`TEXT_SEARCH(TextField1, "query")`,
		`Text_Search(TextField1, "query")`,
	}

	for _, exprStr := range exprs {
		expr, err := ParseExpr(schema, exprStr, nil)
		assert.NoError(t, err, "Should handle case variation: %s", exprStr)
		assert.NotNil(t, expr)
	}
}
