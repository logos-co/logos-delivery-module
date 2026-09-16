# Convenience targets. The module itself is built with Nix — see the README.

.PHONY: docs docs-preview

docs:
	doxygen ./docs/Doxyfile
	$(MAKE) -C docs html

docs-preview:
	./docs/preview.sh --watch
