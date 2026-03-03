// Package main implements a lightweight RSGInterface for AST node → sql_command mapping.
// SimpleRSG uses pure random selection (no MAB, no path feedback) to generate SQL.
package main

import (
	"math"
	"math/rand"
	"strings"

	"github.com/rsg/constant_structs"
	"github.com/rsg/edgeClassifier"
	"github.com/rsg/rsgGenerator"
	"github.com/rsg/yacc"
)

// SimpleRSG is a stripped-down implementation of RSGInterface suitable for
// bulk SQL generation during node-to-sqlcommand mapping discovery.
type SimpleRSG struct {
	allProds                 map[string][]*yacc.ExpressionNode
	allTermProds             map[string][]*yacc.ExpressionNode // rules that expand only to terminals
	allNormProds             map[string][]*yacc.ExpressionNode // rules that are neither term nor comp
	allCompProds             map[string][]*yacc.ExpressionNode // rules containing complex sub-expressions
	allCompNonRecursiveProds map[string][]*yacc.ExpressionNode // complex but non-self-recursive
	rng                      *rand.Rand
	pathId                   int
}

// NewSimpleRSG builds a SimpleRSG from a parsed yacc Tree.
// All grammar edges are loaded and classified for depth-bounded generation.
func NewSimpleRSG(tree *yacc.Tree, seed int64) *SimpleRSG {
	rng := rand.New(rand.NewSource(seed))
	r := &SimpleRSG{
		allProds:                 make(map[string][]*yacc.ExpressionNode),
		allTermProds:             make(map[string][]*yacc.ExpressionNode),
		allNormProds:             make(map[string][]*yacc.ExpressionNode),
		allCompProds:             make(map[string][]*yacc.ExpressionNode),
		allCompNonRecursiveProds: make(map[string][]*yacc.ExpressionNode),
		rng:                      rng,
	}

	// Load all productions from the grammar tree.
	for _, prod := range tree.Productions {
		if existing, ok := r.allProds[prod.Name]; ok {
			_ = existing
			for _, expr := range prod.Expressions {
				// Assign a unique hash for edge coverage (needed by GenerateMySQL).
				expr.UniqueHash = uint32(rng.Intn(65536))
				r.allProds[prod.Name] = append(r.allProds[prod.Name], expr)
			}
		} else {
			// First time seeing this production; assign hashes.
			for _, expr := range prod.Expressions {
				expr.UniqueHash = uint32(rng.Intn(65536))
			}
			r.allProds[prod.Name] = prod.Expressions
		}
	}

	r.classifyEdges()
	return r
}

// classifyEdges categorises each production rule as terminal, complex or normal.
// This mirrors the logic in rsg/EdgeClassifierCommon.go (2-level scan) and is
// self-contained so we do not need to import the package-main code.
func (r *SimpleRSG) classifyEdges() {
	for rootName, rootProds := range r.allProds {
		for _, prod := range rootProds {
			isTerm := true
			isComp := false

			for _, curNode := range prod.Items {
				if isComp {
					break
				}
				// MySQL complex-node heuristic (expr, subquery, joined_table, list …).
				if edgeClassifier.IsMySQLCompNode(rootName, curNode.Value) {
					isComp = true
					isTerm = false
					break
				}
				// Self-recursive rule → complex.
				if curNode.Value == rootName {
					isComp = true
					isTerm = false
					break
				}

				// Check if the child is a non-terminal.
				childProds, childOk := r.allProds[curNode.Value]
				if childOk && len(childProds) > 0 {
					// Not a terminal child — dig one level deeper (mirrors EdgeClassifierCommon.go).
					isTerm = false

					// If ALL grandchild productions contain complex nodes, mark as comp.
					isAllGrandChildComp := true
					for _, childProd := range childProds {
						grandChildComp := false
						for _, grandChildNode := range childProd.Items {
							if edgeClassifier.IsMySQLCompNode(curNode.Value, grandChildNode.Value) {
								grandChildComp = true
							}
							if _, grandOk := r.allProds[grandChildNode.Value]; grandOk {
								isTerm = false
							}
						}
						if !grandChildComp {
							isAllGrandChildComp = false
						}
					}
					if isAllGrandChildComp {
						isComp = true
						isTerm = false
					}
				}
			}

			if isTerm {
				prod.NodeComp = 0
				r.allTermProds[rootName] = append(r.allTermProds[rootName], prod)
			} else if isComp {
				prod.NodeComp = 2
				r.allCompProds[rootName] = append(r.allCompProds[rootName], prod)
			} else {
				prod.NodeComp = 1
				r.allNormProds[rootName] = append(r.allNormProds[rootName], prod)
			}
		}
	}

	// Populate allCompNonRecursiveProds (non-self-recursive rules only).
	for rootName, rootProds := range r.allProds {
		for _, prod := range rootProds {
			isRecursive := false
			for _, item := range prod.Items {
				if item.Value == rootName {
					isRecursive = true
					break
				}
			}
			if !isRecursive {
				r.allCompNonRecursiveProds[rootName] = append(r.allCompNonRecursiveProds[rootName], prod)
			}
		}
	}

	// MySQL special handling: treat non-recursive query_primary rules as norm+comp rules
	// so that depth limiting kicks in correctly (mirrors EdgeClassifierCommon.go).
	r.allNormProds["query_primary"] = append(
		r.allNormProds["query_primary"],
		r.allCompNonRecursiveProds["query_primary"]...,
	)
	r.allCompProds["query_primary"] = append(
		r.allCompProds["query_primary"],
		r.allCompNonRecursiveProds["query_primary"]...,
	)
}

// ---------------------------------------------------------------------------
// RSGInterface implementation
// ---------------------------------------------------------------------------

// PrioritizeParserRules returns candidate production rules for root.
// When depth is exhausted it prefers terminal rules to break recursion.
// Fallback chain when depth ≤ 0: termProds → normProds → compNonRecursiveProds → allProds.
func (r *SimpleRSG) PrioritizeParserRules(root string, _ uint32, depth int) []*yacc.ExpressionNode {
	if depth <= 0 {
		if term := r.allTermProds[root]; len(term) > 0 {
			return term
		}
		if norm := r.allNormProds[root]; len(norm) > 0 {
			return norm
		}
		if nonRec := r.allCompNonRecursiveProds[root]; len(nonRec) > 0 {
			return nonRec
		}
	}
	if all := r.allProds[root]; len(all) > 0 {
		return all
	}
	return nil
}

// MABChooseArm picks a uniformly random production rule from candidates.
func (r *SimpleRSG) MABChooseArm(prods []*yacc.ExpressionNode) *yacc.ExpressionNode {
	if len(prods) == 0 {
		return nil
	}
	return prods[r.rng.Intn(len(prods))]
}

// MarkEdgeCov is a no-op; we do not track coverage in this generator.
func (r *SimpleRSG) MarkEdgeCov(_ uint32, _ uint32) {}

// CheckIsFav always returns false; no favourite-path mechanism here.
func (r *SimpleRSG) CheckIsFav(_ string, _ uint32) bool { return false }

// IsInCompProds reports whether curChosenRule is among the complex productions
// for root (used by GenerateMySQL to decide whether to decrement depth).
func (r *SimpleRSG) IsInCompProds(root string, curChosenRule *yacc.ExpressionNode) bool {
	for _, cp := range r.allCompProds[root] {
		if cp == curChosenRule {
			return true
		}
	}
	return false
}

// Intn returns a uniformly random non-negative integer < n.
func (r *SimpleRSG) Intn(n int) int { return r.rng.Intn(n) }

// Float64 returns a random float; mirrors the distribution in rsg/helperFunc.go.
func (r *SimpleRSG) Float64() float64 {
	v := r.rng.Float64()*2 - 1
	switch r.rng.Intn(10) {
	case 0:
		v = 0
	case 1:
		v = math.Inf(1)
	case 2:
		v = math.Inf(-1)
	case 3:
		v = math.NaN()
	case 4, 5:
		v *= math.Pow10(r.rng.Intn(50))
	case 6, 7:
		v *= math.Pow10(-r.rng.Intn(50))
	}
	return v
}

// GetAllProds returns the full production map.
func (r *SimpleRSG) GetAllProds() map[string][]*yacc.ExpressionNode { return r.allProds }

// GetPathId / SetPathId satisfy the interface; unused in simple mode.
func (r *SimpleRSG) GetPathId() int   { return r.pathId }
func (r *SimpleRSG) SetPathId(id int) { r.pathId = id }

// FormatTokenValue strips a trailing "_P" suffix (Postgres keyword marker).
func (r *SimpleRSG) FormatTokenValue(in string) string {
	if strings.HasSuffix(in, "_P") {
		return in[:len(in)-2]
	}
	return in
}

// GetMappedKeywords returns nil; keyword mapping is not needed for MySQL.
func (r *SimpleRSG) GetMappedKeywords() map[string]interface{} { return nil }

// GetCurFuzzingMode returns FuzzingModeNoFavNoMABNoAcc (mode 3) so that
// GenerateMySQL's depth-limiting branch is active (requires mode < 4).
// Mode 3 disables favourite paths, MAB, and accumulative mutations but
// keeps the depth cut-off that prevents stack overflows.
func (r *SimpleRSG) GetCurFuzzingMode() constant_structs.FuzzingMode {
	return constant_structs.FuzzingModeNoFavNoMABNoAcc
}

// ---------------------------------------------------------------------------
// SQL generation helper
// ---------------------------------------------------------------------------

// GenerateSQL attempts to produce a complete SQL string rooted at root.
// depth controls recursion depth (2 is the default for MySQL in the main RSG).
// Returns an empty string if generation fails or yields only whitespace.
func (r *SimpleRSG) GenerateSQL(root string, depth int) string {
	pathNode := &rsgGenerator.PathNode{
		Id:       r.pathId,
		Parent:   nil,
		Children: []*rsgGenerator.PathNode{},
		IsFav:    false,
	}
	tokens := rsgGenerator.GenerateMySQL(r, root, pathNode, 0, depth, depth)
	if tokens == nil {
		return ""
	}
	s := strings.Join(tokens, " ")
	s = strings.TrimSpace(s)
	// GenerateMySQL can emit the internal marker "_LA" which should be stripped.
	s = strings.ReplaceAll(s, "_LA", "")
	s = strings.TrimSpace(s)
	return s
}
