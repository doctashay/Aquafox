# -*- makefile -*-
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Bytecode cache generation for packaged chrome JS
#
# This makefile fragment can be included during the packaging phase
# to generate .jsc bytecode cache files for chrome JavaScript.
#
# Usage:
#   make -f bytecode_cache.mk generate-caches OBJDIR=... SRCDIR=...

PYTHON ?= python
GENERATE_SCRIPT := $(dir $(lastword $(MAKEFILE_LIST)))/generate_jsc_caches.py

# Default directories
OBJDIR ?= $(error OBJDIR must be specified)
SRCDIR ?= $(error SRCDIR must be specified)
OUTPUT_DIR ?= $(OBJDIR)/dist/bin

.PHONY: generate-caches validate-scripts

generate-caches:
	@echo "Generating bytecode caches..."
	$(PYTHON) $(GENERATE_SCRIPT) \
		--objdir $(OBJDIR) \
		--srcdir $(SRCDIR) \
		--output $(OUTPUT_DIR) \
		--manifest $(OBJDIR)/bytecode_cache_manifest.json \
		--verbose

validate-scripts:
	@echo "Validating chrome scripts compile..."
	$(PYTHON) $(GENERATE_SCRIPT) \
		--objdir $(OBJDIR) \
		--srcdir $(SRCDIR) \
		--validate-only \
		--verbose
