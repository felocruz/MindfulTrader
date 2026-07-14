---
domain: <category/subcategory>          # e.g. sierra_chart/replay, architecture/zmq
intent: <one sentence — what question does reading this answer?>
scope: global                            # global (stable truth) | local (fluid, update often)
tags: [tag1, tag2, tag3]                 # keywords the assembler matches against task queries
source_files:                            # which code files this describes (for staleness checks)
  - src/path/to/file.cpp
last_verified: YYYY-MM-DD               # date last cross-checked against actual code
dependencies: []                         # other chunk slugs this one assumes the reader knows
---

# <Title>

## Why This Exists
<!-- One paragraph: the design decision or constraint that makes this chunk necessary.
     Focus on the WHY, not the WHAT. What would go wrong if the reader didn't know this? -->

## The Invariant / Contract
<!-- The single most important rule or constraint. State it first, explain second. -->

## How It Works
<!-- Functional description with interface definitions co-located with code examples.
     Keep code snippets to the essential call signature + return type + one usage example. -->

## Failure Modes
<!-- What breaks when this is misunderstood or misapplied. -->

## References
<!-- Authoritative source: doc file, Sierra Chart page URL, local header -->
