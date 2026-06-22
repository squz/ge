# 🎯T124 Headless render regression — dev-box only (Metal), no device / ged.
#
# `make render-test`            batch-render fixtures/*.json -> build/render/,
#                               imgdiff each vs fixtures/golden/ (RMS threshold).
# `make update-render-goldens`  re-bless goldens after a deliberate visual change.
#
# Goldens are small (320x240) committed reference PNGs, same convention as
# test/refs/; the imgdiff RMS threshold absorbs minor cross-GPU rasterisation
# differences. Re-bless with `make update-render-goldens` after a visual change.

RENDER_SIZE ?= 320x240

.PHONY: render-test update-render-goldens

render-test: $(APP) $(ge/IMGDIFF)
	@mkdir -p build/render
	@./$(APP) render --batch fixtures/manifest.json --size $(RENDER_SIZE)
	@ok=1; for g in fixtures/golden/*.png; do n=$$(basename $$g); ./$(ge/IMGDIFF) build/render/$$n $$g --threshold=0.03 && echo "  PASS $$n" || { echo "  FAIL $$n"; ok=0; }; done; [ $$ok = 1 ] && echo "render-test: fixtures match goldens"

update-render-goldens: $(APP)
	@mkdir -p build/render fixtures/golden
	@./$(APP) render --batch fixtures/manifest.json --size $(RENDER_SIZE)
	@cp build/render/*.png fixtures/golden/
	@echo "render goldens updated"
