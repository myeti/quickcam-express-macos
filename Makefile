CC      = clang
# Oldest macOS the binaries will run on (Big Sur, the first with Apple Silicon).
MINOS   = -mmacosx-version-min=11.0 -Wunguarded-availability-new
CFLAGS  = -std=c11 -O2 -Wall -Wextra -fblocks $(MINOS)
OBJCFLAGS = -O2 -Wall -Wextra -fobjc-arc $(MINOS)
LDFLAGS = $(MINOS)

DRIVER  = build/usb_darwin.o build/quickcam.o build/isp.o build/spca561_decode.o build/image_io.o
LIBS    = -framework IOKit -framework CoreFoundation -framework CoreGraphics -framework ImageIO

all: build/qcsnap build/qcview build/qcserve

build:
	mkdir -p build

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/spca561_decode.o: src/spca561_decode.c src/spca561_decode.h | build
	$(CC) $(CFLAGS) -Wno-unused-parameter -c $< -o $@

build/qcsnap: build/qcsnap.o $(DRIVER)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

build/page_html.h: src/page.html | build
	cd src && xxd -i page.html > ../build/page_html.h

build/qcserve.o: src/qcserve.c build/page_html.h src/*.h | build
	$(CC) $(CFLAGS) -Ibuild -c $< -o $@

build/qcserve: build/qcserve.o $(DRIVER)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

build/qcview: src/qcview.m $(DRIVER) src/*.h
	$(CC) $(OBJCFLAGS) -o $@ src/qcview.m $(DRIVER) $(LIBS) -framework AppKit -framework QuartzCore

AGENT = $(HOME)/Library/LaunchAgents/local.quickcam.qcserve.plist

# Start qcserve at login (and now). The camera only turns on while the feed is watched.
install-agent: build/qcserve
	mkdir -p $(dir $(AGENT)) $(HOME)/Library/Logs
	-launchctl bootout gui/$$(id -u) $(AGENT) 2>/dev/null
	sed -e 's|@QCSERVE@|$(CURDIR)/build/qcserve|' -e 's|@LOG@|$(HOME)/Library/Logs/qcserve.log|' \
		launchd/local.quickcam.qcserve.plist > $(AGENT)
	launchctl bootstrap gui/$$(id -u) $(AGENT)

uninstall-agent:
	-launchctl bootout gui/$$(id -u) $(AGENT)
	rm -f $(AGENT)

clean:
	rm -rf build

.PHONY: all clean install-agent uninstall-agent
