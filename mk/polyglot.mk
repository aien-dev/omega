# POLYGLOT-0 build and test targets. spec/polyglot-0.md
# Owner: lane F (verifier/bench). Other lanes ask F to add their sources.
.PHONY: test-polyglot bench-polyglot
-include $(wildcard mk/polyglot-*.mk)
