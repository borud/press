.PHONY: help build upload uploadfs monitor upload-monitor upload-all clean fullclean erase info

# Default target: list available targets
help:
	@echo 
	@echo "Available targets:"
	@echo 
	@echo "  build              Build firmware"
	@echo "  upload             Flash firmware via serial"
	@echo "  uploadfs           Flash LittleFS filesystem (web UI)"
	@echo "  upload-all         Flash firmware + filesystem"
	@echo "  monitor            Serial console (115200)"
	@echo "  upload-monitor     Flash firmware and open monitor"
	@echo "  upload-all-monitor Flash everything and open monitor"
	@echo "  clean              Clean build artifacts (keeps sdkconfig)"
	@echo "  fullclean          Nuke build artifacts + sdkconfig"
	@echo "  rebuild            Full clean + build"
	@echo "  erase              Wipe entire flash"
	@echo "  info               Show firmware size and partition table"
	@echo

# Build firmware
build:
	pio run

# Upload firmware only
upload:
	pio run -t upload

# Upload LittleFS filesystem image (data/ directory)
uploadfs:
	pio run -t uploadfs

# Upload firmware + filesystem
upload-all: upload uploadfs

# Serial monitor
monitor:
	pio device monitor

# Upload firmware and start monitor
upload-monitor:
	pio run -t upload -t monitor

# Upload everything (firmware + filesystem) then monitor
upload-all-monitor: upload-all monitor

# Clean build artifacts (keeps sdkconfig)
clean:
	pio run -t clean

# Full clean including sdkconfig (forces rebuild from sdkconfig.defaults)
fullclean:
	rm -f sdkconfig.esp32dev
	rm -rf .pio/build

# Erase entire flash
erase:
	pio run -t erase

# Show partition table and binary sizes
info:
	@echo "=== Firmware size ==="
	@pio run 2>&1 | grep -E "(RAM|Flash):" || true
	@echo ""
	@echo "=== Partition table ==="
	@cat partitions.csv
	@echo ""
	@echo "=== Web assets ==="
	@ls -lh data/ 2>/dev/null || echo "No data/ directory"

# Rebuild from scratch (new sdkconfig + full build)
rebuild: fullclean build
