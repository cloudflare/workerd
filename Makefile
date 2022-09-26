
LATEST_COMPATIBILITY_DATE=$(shell bazel build @capnp-cpp//src/capnp:capnp_tool && bazel-bin/external/capnp-cpp/src/capnp/capnp_tool eval src/workerd/io/compatibility-date.capnp supportedCompatibilityDate)
WORKERD_VERSION=1.$(shell bazel build @capnp-cpp//src/capnp:capnp_tool && bazel-bin/external/capnp-cpp/src/capnp/capnp_tool eval src/workerd/io/compatibility-date.capnp supportedCompatibilityDate | tr -d '-' | tr -d '"').0

platform-bazel-build:
	bazel build -c opt //src/workerd/server:workerd
	mkdir -p "$(NPMDIR)/bin"
	WORKERD_VERSION=$(WORKERD_VERSION) node npm/scripts/bump-version.mjs "$(NPMDIR)/package.json"
	cp bazel-bin/src/workerd/server/workerd $(NPMDIR)/bin/workerd

platform-darwin:
	@$(MAKE) NPMDIR=npm/workerd-darwin-64 platform-bazel-build

platform-darwin-arm64:
	@$(MAKE) NPMDIR=npm/workerd-darwin-arm64 platform-bazel-build

platform-linux:
	@$(MAKE) NPMDIR=npm/workerd-linux-64 platform-bazel-build

platform-linux-arm64:
	@$(MAKE) NPMDIR=npm/workerd-linux-arm64 platform-bazel-build

platform-neutral:
	echo $(WORKERD_VERSION)
	WORKERD_VERSION=$(WORKERD_VERSION) node npm/scripts/bump-version.mjs "npm/workerd/package.json"
	mkdir -p npm/workerd/lib
	mkdir -p npm/workerd/bin
	npx esbuild npm/lib/node-install.ts --outfile=npm/workerd/install.js --bundle --target=node16 --define:LATEST_COMPATIBILITY_DATE="\"$(LATEST_COMPATIBILITY_DATE)\"" --platform=node --external:workerd --log-level=warning
	npx esbuild npm/lib/node-shim.ts --outfile=npm/workerd/bin/workerd --bundle --target=node16  --define:LATEST_COMPATIBILITY_DATE="\"$(LATEST_COMPATIBILITY_DATE)\"" --platform=node --external:workerd --log-level=warning
	npx esbuild npm/lib/node-path.ts --outfile=npm/workerd/lib/main.js --bundle --target=node16  --define:LATEST_COMPATIBILITY_DATE="\"$(LATEST_COMPATIBILITY_DATE)\"" --platform=node --external:workerd --log-level=warning
	WORKERD_VERSION=$(WORKERD_VERSION) node npm/scripts/build-shim-package.mjs

publish-darwin: platform-darwin
	cd npm/workerd-darwin-64 && npm publish

publish-darwin-arm64: platform-darwin-arm64
	cd npm/workerd-darwin-arm64 && npm publish

publish-linux: platform-linux
	cd npm/workerd-linux-64 && npm publish

publish-linux-arm64: platform-linux-arm64
	cd npm/workerd-linux-arm64 && npm publish

publish-neutral: platform-neutral
	cd npm/workerd && npm publish

validate-build:
	@test -n "$(TARGET)" || (echo "The environment variable TARGET must be provided" && false)
	@test -n "$(PACKAGE)" || (echo "The environment variable PACKAGE must be provided" && false)
	@test -n "$(SUBPATH)" || (echo "The environment variable SUBPATH must be provided" && false)
	@echo && echo "🔷 Checking $(SCOPE)$(PACKAGE)"
	@rm -fr validate && mkdir validate
	@$(MAKE) --no-print-directory "$(TARGET)"
	@curl -s "https://registry.npmjs.org/$(SCOPE)$(PACKAGE)/-/$(PACKAGE)-$(WORKERD_VERSION).tgz" > validate/workerd.tgz
	@cd validate && tar xf workerd.tgz
	@ls -l "npm/$(SCOPE)$(PACKAGE)/$(SUBPATH)" "validate/package/$(SUBPATH)" && \
		shasum "npm/$(SCOPE)$(PACKAGE)/$(SUBPATH)" "validate/package/$(SUBPATH)" && \
		cmp "npm/$(SCOPE)$(PACKAGE)/$(SUBPATH)" "validate/package/$(SUBPATH)"
	@rm -fr validate

# This checks that the published binaries are bitwise-identical to the locally-build binaries
validate-builds:
	git fetch --all --tags && git checkout "v$(WORKERD_VERSION)"
	@$(MAKE) --no-print-directory TARGET=platform-darwin PACKAGE=workerd-darwin-64 SUBPATH=bin/workerd validate-build
	@$(MAKE) --no-print-directory TARGET=platform-darwin-arm64 PACKAGE=workerd-darwin-arm64 SUBPATH=bin/workerd validate-build
	@$(MAKE) --no-print-directory TARGET=platform-linux PACKAGE=workerd-linux-64 SUBPATH=bin/workerd validate-build
	@$(MAKE) --no-print-directory TARGET=platform-linux-arm64 PACKAGE=workerd-linux-arm64 SUBPATH=bin/workerd validate-build

clean:
	rm -f npm/workerd/install.js
	rm -rf npm/workerd-darwin-64/bin
	rm -rf npm/workerd-darwin-arm64/bin
	rm -rf npm/workerd-linux-64/bin
	rm -rf npm/workerd-linux-arm64/bin
	rm -rf npm/workerd/bin
	rm -rf npm/workerd/lib
	